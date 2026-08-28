#include "record-test-types.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Sampling is statistical, so the assertions here are deliberately loose about COUNTS and strict about SHAPE.
//
// A tight "expect rate x duration samples" check would fail on a loaded machine and teach everyone to ignore it.
// What must hold exactly: samples carry frames, they carry an anchor, splicing moves them onto the anchored thread,
// and nothing is ever lost.

namespace
{
/// Burns wall-clock time so a sampler has something to catch, without spinning a core flat out.
///
/// The mark is not decoration: the sampler only knows the threads the RECORDER knows, and a thread joins that set by
/// recording something.
/// A thread that has never recorded is invisible to it — see record/sampling.hh.
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
    return cc::stack_capture_from_context_available() && CC_HAS_THREADS;
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

    auto const r = capture_sampled([] { busy_for_secs(0.25); }, {.rate_hz = 500.0});

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

REC_TEST("record/sampling - stopping is synchronous, so nothing arrives afterwards")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        cc::rec::start_sampling({.rate_hz = 1000.0});
        CHECK(cc::rec::is_sampling());
        busy_for_secs(0.1);
        cc::rec::stop_sampling();
        CHECK(!cc::rec::is_sampling());

        cc::rec::flush_blocking();
    }
    auto const during = rl.take();

    // A second window, with the sampler down.
    // stop_sampling joins the thread, so this cannot race.
    cc::rec::recording_listener after_rl;
    {
        scoped_listener const reg(after_rl);
        busy_for_secs(0.05);
        cc::rec::flush_blocking();
    }

    CHECK(count_samples(during) > 0);
    CHECK(count_samples(after_rl.take()) == 0);
}

REC_TEST("record/sampling - splicing moves a sample onto the thread it caught")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled(
        []
        {
            CC_RECORD_MARK("before-the-work");
            busy_for_secs(0.25);
            CC_RECORD_MARK("after-the-work");
        },
        {.rate_hz = 500.0});

    auto const before = count_samples(r);
    REQUIRE(before > 0);

    auto const spliced = r.spliced_samples();

    // Nothing is created and nothing is lost — a sample whose anchor names bytes we do not have stays where it was.
    CHECK(count_samples(spliced) == before);
    CHECK(spliced.count("before-the-work") == 1);
    CHECK(spliced.count("after-the-work") == 1);

    // At least some landed on a thread other than the sampler's, which is the whole point of the anchor.
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

    auto const r = capture_sampled([] { busy_for_secs(0.2); }, {.rate_hz = 500.0});
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

REC_TEST("record/sampling - a scope shortens what a sample has to carry")
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

    auto const unbounded = capture_sampled([] { busy_for_secs(0.2); }, {.rate_hz = 500.0, .stop_at_scope = false});
    auto const bounded = capture_sampled(
        []
        {
            CC_RECORD_SCOPE("sampled-region");
            busy_for_secs(0.2);
        },
        {.rate_hz = 500.0, .stop_at_scope = true});

    REQUIRE(count_samples(unbounded) > 0);
    REQUIRE(count_samples(bounded) > 0);

    // The frames below the scope are what the scope stack already names, so a sample inside one carries fewer.
    CHECK(typical_frames(bounded) < typical_frames(unbounded));
}

REC_TEST("record/sampling - threads the recorder never heard of are sampled too, without an anchor")
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
    auto const with = capture_sampled([] { busy_for_secs(0.25); }, {.rate_hz = 500.0, .include_unknown_threads = true});
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

    auto const without
        = capture_sampled([] { busy_for_secs(0.25); }, {.rate_hz = 500.0, .include_unknown_threads = false});
    CHECK(unknown_count(without) == 0);
}

REC_TEST("record/sampling - the sampler records its own cadence and cost")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_sampled([] { busy_for_secs(0.25); }, {.rate_hz = 500.0});

    // The sampler's own lane says when it ran and what each tick cost, which is what lets a reader judge whether a
    // profile is evenly sampled or aliased against a periodic workload.
    auto const ticks = r.scopes("record.sample_tick");
    CHECK(ticks.size() > 0);

    // Every tick is a closed span with a duration, not a bare marker.
    for (auto const& t : ticks)
        CHECK(!t.is_open);
}

REC_TEST("record/sampling - one tick covers every thread, so a rate is a per-thread rate")
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
    // Deliberately a ratio rather than two sample totals: each run is a fixed wall-clock window, so on a machine that
    // is busy or thermally throttled the two windows deliver different numbers of ticks for reasons that have nothing
    // to do with what is being tested — which made the total-vs-total form fail on a loaded laptop.
    // How many threads ONE tick covers is the same answer however few ticks landed.
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
    auto const wide = capture_sampled([] { busy_for_secs(0.25); }, {.rate_hz = 500.0, .include_unknown_threads = true});
    auto const narrow = capture_sampled([] { busy_for_secs(0.25); },
                                        {.rate_hz = 500.0, .threads_per_tick = 1, .include_unknown_threads = true});

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

    cc::println("");
    cc::println("  unknown   ticks   samples   mean tick us   total tick ms   sampler load");

    for (auto const unknown : {false, true})
    {
        auto const r
            = capture_sampled([] { busy_for_secs(0.5); }, {.rate_hz = 1000.0, .include_unknown_threads = unknown});

        auto const ticks = r.scopes("record.sample_tick");

        auto total = 0.0;
        for (auto const& s : ticks)
            total += s.duration_secs();

        cc::println("  {:7}   {:5}   {:7}   {:12.1f}   {:13.2f}   {:11.1f}%", unknown ? "yes" : "no", ticks.size(),
                    count_samples(r), ticks.empty() ? 0.0 : total / f64(ticks.size()) * 1e6, total * 1e3,
                    total / 0.5 * 100);
    }
}

REC_TEST("record/sampling - unknown threads are off unless asked for")
{
    // The default costs the profile and not just the CPU, so it has to be the quiet one.
    CHECK(!cc::rec::sampling_config{}.include_unknown_threads);
}

REC_TEST("record/sampling - the configuration can change while the sampler runs")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0, .include_unknown_threads = false});
            CHECK(!cc::rec::current_sampling_config().include_unknown_threads);

            busy_for_secs(0.1);

            {
                // What a checkbox in a profiler window does, and what a test narrows with.
                // The sampler must not have to be stopped for this: a stop-and-restart would lose every sample taken
                // between the two, which is exactly the stretch somebody turning a knob is looking at.
                cc::rec::sampling_override const all_threads({.rate_hz = 500.0, .include_unknown_threads = true});
                CHECK(cc::rec::current_sampling_config().include_unknown_threads);

                busy_for_secs(0.15);
            }

            // Restored, which is what makes it usable around a suspicious region rather than for a whole run.
            CHECK(!cc::rec::current_sampling_config().include_unknown_threads);
            busy_for_secs(0.1);
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
