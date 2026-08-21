#include "record-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/splicing_listener.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Live splicing is the offline splice run against an incomplete recording, so the bar it has to clear is that it
// agrees with the offline one about everything it manages to place — and loses nothing about the rest.
//
// Nothing here asserts HOW MANY samples get placed live.
// That depends on how the batches happened to fall, which is the recorder's business and a loaded machine's.
// What must hold is that the total never changes.

namespace
{
[[nodiscard]] bool can_sample()
{
    return cc::stack_capture_from_context_available() && CC_HAS_THREADS;
}

CC_DONT_INLINE void burn_for_secs(f64 secs)
{
    CC_RECORD_SCOPE("spliced-region");

    auto const start = cc::current_time_steady_secs();
    u64 volatile sink = 0;
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink + u64(i);
}

/// Samples that carry an anchor at all.
///
/// A sample of a thread the recorder never heard of has no stream to be anchored into, so it is unplaceable by
/// construction rather than by any failure to place it — and with include_unknown_threads on, most samples are those.
[[nodiscard]] isize anchored_samples(cc::rec::recording const& r)
{
    isize n = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::sample)
                return;
            if (e.field_as_u64("thread_index").value_or(cc::rec::impl::sample_unknown_thread)
                != cc::rec::impl::sample_unknown_thread)
                ++n;
        });
    return n;
}

[[nodiscard]] isize samples_in(cc::rec::recording const& r)
{
    return r.count_of_kind(cc::rec::event_kind::sample);
}

/// How many samples sit in a block that also carries something else.
///
/// That is what "placed" MEANS here: a sample in the sampler's own stream is alone with its siblings, and a sample
/// that found its target is surrounded by that thread's events.
[[nodiscard]] isize samples_among_other_events(cc::rec::recording const& r)
{
    isize n = 0;
    for (auto const& b : r.blocks())
    {
        isize samples = 0;
        isize others = 0;
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            if ((*it).kind() == cc::rec::event_kind::sample)
                ++samples;
            else
                ++others;
        }
        if (others > 0)
            n += samples;
    }
    return n;
}
} // namespace

REC_TEST("record/splicing - a listener downstream of the splicer sees every event")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;
    cc::rec::splicing_listener splicer(downstream);

    cc::rec::recording_listener raw;
    {
        scoped_listener const a(raw);
        scoped_listener const b(splicer);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            burn_for_secs(0.25);
        }
        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const spliced = downstream.take();
    auto const direct = raw.take();

    REQUIRE(samples_in(direct) > 0);

    // Moving a sample must never lose or duplicate one, which is the only thing splicing is allowed to be wrong
    // about and the thing that would be silent.
    CHECK(samples_in(spliced) == samples_in(direct));
    CHECK(spliced.event_count() == direct.event_count());
}

REC_TEST("record/splicing - samples end up among the events of the thread they anchor into")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;
    cc::rec::splicing_listener splicer(downstream);
    {
        scoped_listener const reg(splicer);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            burn_for_secs(0.25);
        }
        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const spliced = downstream.take();
    REQUIRE(samples_in(spliced) > 0);

    // The whole point of doing this live: a sample sitting in the sampled thread's stream is one a consumer can read
    // the scope stack and the ambient context of, and one in the sampler's stream is not.
    CHECK(samples_among_other_events(spliced) > 0);
    CHECK(splicer.spliced_count() > 0);
}

REC_TEST("record/splicing - live placement agrees with the offline splice")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;
    cc::rec::splicing_listener splicer(downstream);

    cc::rec::recording_listener raw;
    {
        scoped_listener const a(raw);
        scoped_listener const b(splicer);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            burn_for_secs(0.25);
        }
        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const live = downstream.take();
    auto const offline = raw.take().spliced_samples();

    REQUIRE(samples_in(offline) > 0);
    CHECK(samples_in(live) == samples_in(offline));

    // The live splicer sees an incomplete recording, so it may place FEWER — never more, and never a different total.
    // A sample it could not place keeps the position it was recorded at, which is what the offline splice does too.
    CHECK(samples_among_other_events(live) <= samples_among_other_events(offline));
}

REC_TEST("record/splicing - splicing an already-spliced stream changes nothing")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;
    cc::rec::splicing_listener splicer(downstream);
    {
        scoped_listener const reg(splicer);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            burn_for_secs(0.2);
        }
        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const once = downstream.take();
    REQUIRE(samples_in(once) > 0);

    // Idempotence is what makes it safe to splice without knowing whether someone already did.
    auto const twice = once.spliced_samples();
    CHECK(samples_in(twice) == samples_in(once));
    CHECK(twice.event_count() == once.event_count());

    // The layout, because that is what "already spliced" is about: an offline splice run over a LIVE-spliced recording
    // must leave every sample the listener placed exactly where the listener placed it.
    CHECK(event_layout(twice) == event_layout(once));
    CHECK(samples_among_other_events(twice) == samples_among_other_events(once));
}

REC_TEST("record/splicing - a stream with no samples passes straight through")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;
    cc::rec::splicing_listener splicer(downstream);
    {
        scoped_listener const reg(splicer);
        for (isize i = 0; i < 500; ++i)
            CC_RECORD("splice-passthrough", i);

        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const out = downstream.take();

    // The splicer is registered whether or not anyone is sampling, so it must cost nothing and change nothing then.
    CHECK(out.count("splice-passthrough") == 500);
    CHECK(splicer.spliced_count() == 0);
    CHECK(splicer.unplaced_count() == 0);
}

REC_TEST("record/splicing - nothing is stranded when the hold expires")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener downstream;

    // One batch of patience, so a sample that misses its target is given up on almost immediately.
    cc::rec::splicing_listener splicer(downstream, {.max_hold_batches = 1});

    cc::rec::recording_listener raw;
    {
        scoped_listener const a(raw);
        scoped_listener const b(splicer);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            burn_for_secs(0.25);
        }
        cc::rec::flush_blocking();
    }
    splicer.flush();

    auto const out = downstream.take();
    auto const direct = raw.take();

    REQUIRE(samples_in(direct) > 0);

    // Giving up must forward, never drop: a sample nobody could place is still a sample of something.
    CHECK(samples_in(out) == samples_in(direct));
    CHECK(splicer.spliced_count() + splicer.unplaced_count() >= samples_in(direct));
}

TEST("record/splicing - how much a live splice places", nx::config::manual, nx::config::exclusive(), nx::config::owns_recorder)
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    cc::println("");
    cc::println("  hold   samples   anchored   live placed   offline placed");

    for (auto const hold : {1, 2, 4, 8})
    {
        cc::rec::recording_listener downstream;
        cc::rec::splicing_listener splicer(downstream, {.max_hold_batches = hold});

        cc::rec::recording_listener raw;
        {
            scoped_listener const a(raw);
            scoped_listener const b(splicer);
            {
                cc::rec::sampling_scope const sampling({.rate_hz = 1000.0});
                burn_for_secs(0.3);
            }
            cc::rec::flush_blocking();
        }
        splicer.flush();

        auto const live = downstream.take();
        auto const offline = raw.take().spliced_samples();

        cc::println("  {:4}   {:7}   {:8}   {:11}   {:14}", hold, samples_in(live), anchored_samples(live),
                    samples_among_other_events(live), samples_among_other_events(offline));
    }
}
