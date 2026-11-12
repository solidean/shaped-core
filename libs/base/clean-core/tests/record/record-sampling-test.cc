#include "record-test-types.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#endif

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Sampling is statistical, so the assertions here are deliberately loose about COUNTS and strict about SHAPE.
//
// A tight "expect rate x duration samples" check would fail on a loaded machine and teach everyone to ignore it.
// What must hold exactly: samples carry frames, they carry an anchor, splicing moves them onto the anchored thread,
// and nothing is ever lost.

namespace
{
/// Stays busy until the running sampler has taken `samples` more samples, and returns the seconds that took.
///
/// The mark is not decoration: the sampler only knows the threads the RECORDER knows, and a thread joins that set by
/// recording something.
/// A thread that has never recorded is invisible to it — see record/sampling.hh.
CC_DONT_INLINE f64 busy_until_sampled(u64 samples)
{
    CC_RECORD_MARK("busy");
    return burn_until_sampled(samples);
}

/// Counts the drained samples of threads the recorder has no stream for.
struct unknown_sample_counter final : cc::rec::listener
{
    isize count = 0;

    void on_chunk(cc::rec::chunk_view const& view) override
    {
        for (auto it = view.begin(); it != view.end(); ++it)
            if ((*it).kind() == cc::rec::event_kind::sample
                && (*it).field_as_u64("thread_index").value_or(0) == cc::rec::impl::sample_unknown_thread)
                ++count;
    }
};

/// Stays busy until the sampler has caught a thread the recorder never heard of, then for `samples` more samples.
/// Returns the seconds until that first catch.
///
/// Unknown threads are discovered on a wall-clock interval after the sampler starts, so no sample count says one was
/// reached.
/// Watching for one means draining mid-run, so the surrounding capture is flushed more than once.
CC_DONT_INLINE f64 busy_until_unknown_sampled(u64 samples)
{
    unknown_sample_counter counter;
    auto const start = cc::current_time_steady_secs();
    {
        scoped_listener const reg(counter);
        while (counter.count == 0 && cc::current_time_steady_secs() - start < 5.0)
        {
            busy_until_sampled(10);
            cc::rec::flush_blocking();
        }
    }
    auto const discovered_secs = cc::current_time_steady_secs() - start;
    busy_until_sampled(samples);
    return discovered_secs;
}

/// Burns a fixed stretch, for the questions no sample count can end: that something is NOT sampled.
CC_DONT_INLINE void busy_for_secs(f64 secs)
{
    CC_RECORD_MARK("busy");

    auto const start = cc::current_time_steady_secs();
    u64 volatile sink = 0;
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink + u64(i);
}

/// Whether this build can sample at all.
/// Two independent reasons it might not: no way to walk a foreign thread, and no threads to run a sampler on.
[[nodiscard]] bool sampling_possible()
{
    return cc::rec::sampling_available();
}

[[nodiscard]] isize count_samples(cc::rec::recording const& r)
{
    return r.count_of_kind(cc::rec::event_kind::sample);
}

/// Captures everything recorded while `body` runs, with sampling on.
cc::rec::recording capture_sampled(cc::function_ref<void()> body, cc::rec::sampling_config const& cfg)
{
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        {
            cc::rec::sampling_scope const sampling(cfg);
            body();
        }
        cc::rec::flush_blocking();
    }
    return rl.take();
}
} // namespace

REC_TEST("record/sampling - a sampler catches the running thread and names where it was")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled([] { busy_until_sampled(20); }, {.rate_hz = 2000.0});

    auto const samples = count_samples(r);
    REQUIRE(samples > 0);

    // Every sample carries frames and an anchor; a sample with neither would be a sample of nothing.
    isize with_frames = 0;
    isize with_anchor = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::sample)
                return;

            if (!e.field_as_u64_array("frames").empty())
                ++with_frames;
            if (e.field_as_u64("chunk_seq").has_value())
                ++with_anchor;
        });

    CHECK(with_frames == samples);
    CHECK(with_anchor == samples);
}

namespace
{
/// What a sink saw: samples per owner, and whether each carried frames.
struct sink_tally
{
    cc::mutex<cc::map<u64, isize>> by_owner;
    cc::atomic<isize> without_frames = 0;

    static void take(cc::rec::sample_view const& s, void* user)
    {
        auto& self = *static_cast<sink_tally*>(user);
        if (s.frames.empty())
            self.without_frames.fetch_add(1, cc::memory_order_relaxed);
        self.by_owner.lock([&](cc::map<u64, isize>& m) { m[s.owner] += 1; });
    }

    [[nodiscard]] isize of(u64 owner)
    {
        return by_owner.lock(
            [&](cc::map<u64, isize>& m)
            {
                auto const* n = m.get_ptr(owner);
                return n == nullptr ? isize(0) : *n;
            });
    }
};
} // namespace

REC_TEST("record/sampling - a sink receives each sample with the owner its thread was under")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    sink_tally tally;
    auto const owner = cc::rec::new_trace_id();
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 2000.0, .sink = &sink_tally::take, .sink_user = &tally});
            cc::rec::owner_scope const under(owner);
            busy_until_sampled(20);
        }
        cc::rec::flush_blocking();
    }

    // Every sample went to the sink, and the stream got none of them.
    CHECK(tally.of(u64(owner)) > 0);
    CHECK(tally.without_frames.load() == 0);
    CHECK(count_samples(rl.take()) == 0);
}

REC_THOROUGH_TEST("record/sampling - stopping is synchronous, so nothing arrives afterwards")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    auto sampled_secs = 0.0;
    {
        scoped_listener const reg(rl);

        cc::rec::start_sampling({.rate_hz = 2000.0});
        CHECK(cc::rec::is_sampling());
        sampled_secs = busy_until_sampled(10);
        cc::rec::stop_sampling();
        CHECK(!cc::rec::is_sampling());

        cc::rec::flush_blocking();
    }
    auto const during = rl.take();

    // A second window, with the sampler down.
    // stop_sampling joins the thread, so this cannot race.
    // Twice as long as the running sampler needed for its samples, so one that survived the stop would show up here.
    cc::rec::recording_listener after_rl;
    {
        scoped_listener const reg(after_rl);
        busy_for_secs(2 * sampled_secs);
        cc::rec::flush_blocking();
    }

    CHECK(count_samples(during) > 0);
    CHECK(count_samples(after_rl.take()) == 0);
}

REC_THOROUGH_TEST("record/sampling - splicing moves a sample onto the thread it caught")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled(
        []
        {
            CC_RECORD_MARK("before-the-work");
            busy_until_sampled(20);
            CC_RECORD_MARK("after-the-work");
        },
        {.rate_hz = 2000.0});

    auto const before = count_samples(r);
    REQUIRE(before > 0);

    auto const spliced = r.spliced_samples();

    // Nothing is created and nothing is lost — a sample whose anchor names bytes we do not have stays where it was.
    CHECK(count_samples(spliced) == before);
    CHECK(spliced.count("before-the-work") == 1);
    CHECK(spliced.count("after-the-work") == 1);

    // The anchor put at least some on a thread other than the sampler's.
    isize moved = 0;
    spliced.for_each_event(
        [&](cc::rec::chunk_view const& v, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::sample && v.thread.name != "cc-sample")
                ++moved;
        });

    CHECK(moved > 0);
}

REC_TEST("record/sampling - splicing twice changes nothing")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled([] { busy_until_sampled(20); }, {.rate_hz = 2000.0});
    REQUIRE(count_samples(r) > 0);

    auto const once = r.spliced_samples();
    auto const twice = once.spliced_samples();

    // Idempotence is what makes this safe to run in a pipeline that may already have run it.
    CHECK(count_samples(twice) == count_samples(once));
    CHECK(twice.event_count() == once.event_count());

    // On the LAYOUT, not on the counts.
    // Splicing a second time used to strip every sample back out and re-append it in a trailing block, which moved
    // each one out of the scope it had been placed inside while leaving every count identical.
    CHECK(event_layout(twice) == event_layout(once));
}

REC_THOROUGH_TEST("record/sampling - a scope shortens what a sample has to carry")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    // What a TYPICAL sample carried, as a median over the run.
    //
    // Never the deepest sample: a scope bounds the walk at its OUTER end, so a sample that lands inside a deep call
    // still carries every frame below the scope.
    // Two runs' maxima are therefore two independent luckiest landings, and one of those inverts the comparison while
    // the distributions stay far apart.
    auto const typical_frames = [&](cc::rec::recording const& r)
    {
        cc::vector<isize> depths;
        r.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.kind() == cc::rec::event_kind::sample)
                    depths.push_back(e.field_as_u64_array("frames").size());
            });

        REQUIRE(!depths.empty());
        cc::sort(depths);
        return depths[depths.size() / 2];
    };

    auto const unbounded = capture_sampled([] { busy_until_sampled(30); }, {.rate_hz = 2000.0, .stop_at_scope = false});
    auto const bounded = capture_sampled(
        []
        {
            CC_RECORD_SCOPE("sampled-region");
            busy_until_sampled(30);
        },
        {.rate_hz = 2000.0, .stop_at_scope = true});

    REQUIRE(count_samples(unbounded) > 0);
    REQUIRE(count_samples(bounded) > 0);

    // The frames below the scope are what the scope stack already names, so a sample inside one carries fewer.
    CHECK(typical_frames(bounded) < typical_frames(unbounded));
}

REC_THOROUGH_TEST("record/sampling - threads the recorder never heard of are sampled too, without an anchor")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const unknown_count = [](cc::rec::recording const& r)
    {
        isize n = 0;
        r.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.kind() != cc::rec::event_kind::sample)
                    return;
                if (u32(e.field_as_u64("thread_index").value_or(0)) == cc::rec::impl::sample_unknown_thread)
                    ++n;
            });
        return n;
    };

    // A process always has threads nobody recorded through — the CRT's, the debugger's, ours before they record.
    // Those are exactly the threads a profiler is looking for, and the recorder cannot see them.
    auto discovered_secs = 0.0;
    auto const with = capture_sampled([&] { discovered_secs = busy_until_unknown_sampled(20); },
                                      {.rate_hz = 2000.0, .include_unknown_threads = true});
    CHECK(unknown_count(with) > 0);

    // Every sample carries a native id, whether or not it carries an anchor.
    isize with_tid = 0;
    with.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::sample && e.field_as_u64("native_tid").value_or(0) != 0)
                ++with_tid;
        });
    CHECK(with_tid == count_samples(with));

    // Longer than the run above needed to catch one, so a sampler ignoring the flag would have caught one here too.
    auto const without = capture_sampled([&] { busy_for_secs(1.5 * discovered_secs); },
                                         {.rate_hz = 2000.0, .include_unknown_threads = false});
    CHECK(unknown_count(without) == 0);
}

REC_TEST("record/sampling - the sampler records its own cadence and cost")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled([] { busy_until_sampled(10); }, {.rate_hz = 2000.0});

    // The sampler's own lane says when it ran and what each tick cost, which is what lets a reader judge whether a
    // profile is evenly sampled or aliased against a periodic workload.
    auto const ticks = r.scopes("record.sample_tick");
    CHECK(ticks.size() > 0);

    // Every tick is a closed span with a duration, not a bare marker.
    for (auto const& t : ticks)
        CHECK(!t.is_open);
}

REC_THOROUGH_TEST("record/sampling - one tick covers every thread, so a rate is a per-thread rate")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const per_thread = [](cc::rec::recording const& r)
    {
        cc::map<u64, isize> counts;
        r.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.kind() == cc::rec::event_kind::sample)
                    ++counts[e.field_as_u64("native_tid").value_or(0)];
            });
        return counts;
    };

    // Samples PER TICK, which is the property threads_per_tick actually decides.
    //
    // Deliberately a ratio rather than two sample totals: each run ends once it has taken its samples, so the totals
    // are whatever the runs asked for and say nothing about coverage.
    // How many threads ONE tick covers is the same answer however few ticks landed.
    // The wide run asks for many more, since each of its ticks spends a sample on every thread in the process.
    // It also waits for the unknown threads to be discovered, since the ticks before that cover only the known ones.
    auto const samples_per_tick = [](cc::rec::recording const& r)
    {
        auto const ticks = r.scopes("record.sample_tick").size();
        return ticks > 0 ? f64(count_samples(r)) / f64(ticks) : 0.0;
    };

    // Covering every thread per tick is what makes rate_hz mean what a profiler user expects.
    // One thread per tick divides the rate by however many threads exist, which for a frame's worth of ticks is a
    // handful each.
    // Unknown threads on explicitly: the subject here is threads_per_tick, and only one thread in this test records
    // anything, so the recorder's own set gives nothing to divide.
    auto const wide
        = capture_sampled([] { busy_until_unknown_sampled(200); }, {.rate_hz = 2000.0, .include_unknown_threads = true});
    auto const narrow = capture_sampled([] { busy_until_sampled(30); },
                                        {.rate_hz = 2000.0, .threads_per_tick = 1, .include_unknown_threads = true});

    auto const wide_counts = per_thread(wide);

    REQUIRE(wide_counts.size() > 1); // more than one thread was sampled at all
    REQUIRE(wide.scopes("record.sample_tick").size() > 0);
    REQUIRE(narrow.scopes("record.sample_tick").size() > 0);

    // At most one, since that is the cap; below one when a tick found its target unsampleable.
    CHECK(samples_per_tick(narrow) <= 1.0);

    // And more than one, which is the whole claim: a tick covers every thread rather than one of them.
    CHECK(samples_per_tick(wide) > 1.0);
    CHECK(samples_per_tick(wide) > samples_per_tick(narrow));
}

TEST("record/sampling - what sampling unknown threads costs",
     nx::config::manual,
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    if (!sampling_possible())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    // A fixed window is fine here because nothing is asserted: a shorter one only makes the printed load noisier.
    auto const window_secs = nx::is_thorough() ? 0.5 : 0.05;

    cc::println("");
    cc::println("  unknown   ticks   samples   mean tick us   total tick ms   sampler load");

    for (auto const unknown : {false, true})
    {
        auto const r = capture_sampled([&] { busy_for_secs(window_secs); },
                                       {.rate_hz = 1000.0, .include_unknown_threads = unknown});

        auto const ticks = r.scopes("record.sample_tick");

        auto total = 0.0;
        for (auto const& s : ticks)
            total += s.duration_secs();

        cc::println("  {:7}   {:5}   {:7}   {:12.1f}   {:13.2f}   {:11.1f}%", unknown ? "yes" : "no", ticks.size(),
                    count_samples(r), ticks.empty() ? 0.0 : total / f64(ticks.size()) * 1e6, total * 1e3,
                    total / window_secs * 100);
    }
}

REC_TEST("record/sampling - unknown threads are off unless asked for")
{
    // The default costs the profile and not just the CPU, so it has to be the quiet one.
    CHECK(!cc::rec::sampling_config{}.include_unknown_threads);
}

REC_THOROUGH_TEST("record/sampling - the configuration can change while the sampler runs")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 2000.0, .include_unknown_threads = false});
            CHECK(!cc::rec::current_sampling_config().include_unknown_threads);

            busy_until_sampled(5);

            {
                // What a checkbox in a profiler window does, and what a test narrows with.
                // The sampler must not have to be stopped for this: a stop-and-restart would lose every sample taken
                // between the two, which is exactly the stretch somebody turning a knob is looking at.
                cc::rec::sampling_override const all_threads({.rate_hz = 2000.0, .include_unknown_threads = true});
                CHECK(cc::rec::current_sampling_config().include_unknown_threads);

                busy_until_unknown_sampled(5);
            }

            // Restored, which is what makes it usable around a suspicious region rather than for a whole run.
            CHECK(!cc::rec::current_sampling_config().include_unknown_threads);
            busy_until_sampled(5);
        }
        cc::rec::flush_blocking();
    }

    auto const r = rl.take();
    REQUIRE(count_samples(r) > 0);

    // The override was live, so the run holds samples of threads that have no stream to anchor into — which a run
    // with the flag off throughout could not produce.
    isize unanchored = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::sample)
                return;
            if (e.field_as_u64("thread_index").value_or(0) == cc::rec::impl::sample_unknown_thread)
                ++unanchored;
        });
    CHECK(unanchored > 0);
}

REC_TEST("record/sampling - a live rate change is picked up too")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    cc::rec::sampling_scope const sampling({.rate_hz = 200.0});
    CHECK(cc::rec::current_sampling_config().rate_hz == 200.0);

    // Everything is live, not just the flag that motivated it — a config read once per tick has no reason to make
    // one field special.
    cc::rec::reconfigure_sampling({.rate_hz = 900.0, .max_frames = 8});
    CHECK(cc::rec::current_sampling_config().rate_hz == 900.0);
    CHECK(cc::rec::current_sampling_config().max_frames == 8);
}

REC_TEST("record/sampling - a platform that cannot walk a foreign thread refuses to sample")
{
    if (cc::rec::sampling_available())
        SKIP("this platform can walk a foreign thread, so there is nothing to refuse");

    rec_fixture const fixture(deterministic_config());

    // The refusal is the behaviour, not a fallback.
    //
    // wasm is the case this exists for: capturing a stack works there now, so the old gate of "can we capture at
    // all" no longer separates the two — and a sampler that quietly sampled only the thread that started it would
    // report a profile of the wrong thing.
    cc::rec::start_sampling({.rate_hz = 1000.0});
    CHECK(!cc::rec::is_sampling());

    // And it is still safe to stop something that never started.
    cc::rec::stop_sampling();
    CHECK(!cc::rec::is_sampling());
}

// The OS's own unwind lookup takes the loader's function-table lock, which a thread mapping a DLL holds exclusively.
// A sampler that used it would suspend such a thread mid-load and then wait for its lock forever, so a regression
// here HANGS rather than fails: the watchdog's report is where it shows.
REC_TEST("record/sampling - a thread sampled while it loads DLLs is walked rather than waited on")
{
#if !defined(_WIN32)
    SKIP("only Windows walks a suspended thread from outside");
#else
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    // One this process has not loaded, so every load maps it again and every free unmaps it.
    auto const* dll = static_cast<wchar_t const*>(nullptr);
    for (auto const* candidate : {L"msimg32.dll", L"wtsapi32.dll", L"riched20.dll", L"mscms.dll"})
        if (dll == nullptr && ::GetModuleHandleW(candidate) == nullptr)
            dll = candidate;
    if (dll == nullptr)
        SKIP("every candidate DLL is already loaded, so no load would take the lock");

    rec_fixture const fixture(deterministic_config());

    auto loads = 0;
    auto const r = capture_sampled(
        [&]
        {
            CC_RECORD_MARK("loading");
            auto const target = cc::rec::sampling_statistics().taken + 100;
            auto const start = cc::current_time_steady_secs();
            while (cc::rec::sampling_statistics().taken < target && cc::current_time_steady_secs() - start < 10.0)
            {
                if (auto* const module = ::LoadLibraryW(dll); module != nullptr)
                    ::FreeLibrary(module);
                ++loads;
            }
        },
        {.rate_hz = 2000.0, .stop_at_scope = false});

    CHECK(loads > 0);
    CHECK(count_samples(r) > 0);
#endif
}

REC_TEST("record/sampling - each tick tells a thread that ran from one the OS kept off the CPU, and whether it was in "
         "a scope")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");
#if !defined(_WIN32) && !defined(__APPLE__)
    SKIP("record.thread_run is recorded on Windows and macOS only so far");
#else
    rec_fixture const fixture(deterministic_config());

    // a thread the recorder knows, which then sleeps until told to stop
    auto stop = cc::atomic<bool>(false);
    auto sleeper_tid = cc::atomic<u64>(0);
    auto const r = capture_sampled(
        [&]
        {
            auto sleeper = cc::native_thread::start(
                [&]
                {
                    CC_RECORD_MARK("sleeper");
                    sleeper_tid.store(cc::native_thread_id(), cc::memory_order_release);
                    // asleep inside a scope, as a task blocked in its work is
                    CC_RECORD_SCOPE("sleeper waits");
                    while (!stop.load(cc::memory_order_acquire))
                        cc::this_thread_sleep_secs(0.001);
                });
            while (sleeper_tid.load(cc::memory_order_acquire) == 0)
                cc::this_thread_yield();
            busy_until_sampled(40);
            stop.store(true, cc::memory_order_release);
            sleeper.join();
        },
        {.rate_hz = 2000.0});

    auto const busy_tid = cc::native_thread_id();
    auto busy = cc::vector<f64>();
    auto asleep = cc::vector<f64>();
    auto asleep_in_scope = isize(0);
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::value || e.name() != cc::string_view("record.thread_run"))
                return;
            auto const interval = e.field_as_u64("interval").value_or(0);
            if (interval == 0)
                return;
            auto const share = f64(e.field_as_u64("ran").value_or(0)) / f64(interval);
            auto const tid = e.field_as_u64("native_tid").value_or(0);
            if (tid == busy_tid)
                busy.push_back(share);
            else if (tid == sleeper_tid.load(cc::memory_order_acquire))
            {
                asleep.push_back(share);
                asleep_in_scope += e.field_as_u64("in_scope").value_or(0) != 0 ? 1 : 0;
            }
        });
    REQUIRE(busy.size() >= 10);
    REQUIRE(asleep.size() >= 10);
    cc::sort(busy);
    cc::sort(asleep);
    // loose on a loaded machine, strict about which side each median is on
    CHECK(busy[busy.size() / 2] > 0.5);
    CHECK(asleep[asleep.size() / 2] < 0.2);
    // the sleeper's scope is open for all but its first ticks
    CHECK(2 * asleep_in_scope > asleep.size());
#endif
}
