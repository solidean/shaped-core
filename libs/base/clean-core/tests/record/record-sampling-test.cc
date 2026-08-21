#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
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
}

REC_TEST("record/sampling - a scope shortens what a sample has to carry")
{
    if (!sampling_possible())
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture(deterministic_config());

    auto const deepest = [&](cc::rec::recording const& r)
    {
        isize longest = 0;
        r.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.kind() == cc::rec::event_kind::sample)
                    longest = cc::max(longest, e.field_as_u64_array("frames").size());
            });
        return longest;
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
    CHECK(deepest(bounded) < deepest(unbounded));
}
