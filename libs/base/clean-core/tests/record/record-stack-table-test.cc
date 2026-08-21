#include "record-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/stack_table.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Interning is a size optimization that must be invisible, so what is asserted here is exactly that: a consumer going
// through the table cannot tell which shape a sample used.
//
// The win itself is assertable too, and is the reason this exists — many samples, few distinct stacks.

namespace
{
[[nodiscard]] bool can_sample()
{
    return cc::stack_capture_from_context_available() && CC_HAS_THREADS;
}

/// Burns time at the bottom of a deliberately deep stack, so the sampler sees something worth interning.
///
/// The depth is the point: a shallow stack is not interned by design, so a test that burned at the top would prove
/// nothing about a mechanism that never ran.
CC_DONT_INLINE void burn_at_depth(int depth, f64 secs)
{
    if (depth > 0)
    {
        burn_at_depth(depth - 1, secs);

        u64 volatile sink = u64(depth); // defeats the tail call, which would flatten the stack this test needs
        (void)sink;
        return;
    }

    CC_RECORD_MARK("deep");

    auto const start = cc::current_time_steady_secs();
    u64 volatile sink = 0;
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink + u64(i);
}

cc::rec::recording capture_deep(cc::rec::sampling_config const& cfg)
{
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        {
            cc::rec::sampling_scope const sampling(cfg);
            burn_at_depth(24, 0.25);
        }
        cc::rec::flush_blocking();
    }
    return rl.take();
}

[[nodiscard]] isize count_of(cc::rec::recording const& r, cc::rec::event_kind k)
{
    return r.count_of_kind(k);
}

[[nodiscard]] isize count_interned(cc::rec::recording const& r)
{
    isize n = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::sample && e.has_interned_stack())
                ++n;
        });
    return n;
}
} // namespace

REC_TEST("record/stacks - interning off writes every stack out, and defines none")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 0});
    REQUIRE(count_of(r, cc::rec::event_kind::sample) > 0);

    CHECK(count_interned(r) == 0);
    CHECK(count_of(r, cc::rec::event_kind::stack_definition) == 0);

    // The table is empty and the accessor still works, which is what lets a consumer use one unconditionally.
    cc::rec::stack_table const stacks(r);
    CHECK(stacks.empty());
    CHECK(stacks.size() == 0);
    CHECK(stacks.total_frames() == 0);
}

REC_TEST("record/stacks - a deep stack is interned, and resolves back to its frames")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});
    REQUIRE(count_of(r, cc::rec::event_kind::sample) > 0);

    auto const interned = count_interned(r);
    REQUIRE(interned > 0);

    cc::rec::stack_table const stacks(r);

    // Every definition is in the table, and every interned sample resolves through it.
    CHECK(stacks.size() == count_of(r, cc::rec::event_kind::stack_definition));

    isize resolved = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::sample || !e.has_interned_stack())
                return;

            auto const frames = stacks.frames_of(e);
            if (frames.size() >= 4)
                ++resolved;
        });

    // An id that does not resolve is worse than no interning at all: the sample is still there and says nothing.
    CHECK(resolved == interned);
}

REC_TEST("record/stacks - the same stack is written once however often it is sampled")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});

    auto const interned = count_interned(r);
    auto const definitions = count_of(r, cc::rec::event_kind::stack_definition);
    REQUIRE(interned > 0);

    // The whole point: one burn loop sampled hundreds of times is a handful of distinct stacks, not hundreds.
    // A ratio rather than a count, because how many samples land is the machine's business.
    CHECK(definitions <= interned);
    if (interned >= 20)
        CHECK(definitions < interned);
}

REC_TEST("record/stacks - a sample reads the same whichever shape it used")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const inline_run = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 0});
    auto const interned_run = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});

    cc::rec::stack_table const inline_stacks(inline_run);
    cc::rec::stack_table const interned_stacks(interned_run);

    auto const deepest = [](cc::rec::recording const& r, cc::rec::stack_table const& t)
    {
        isize best = 0;
        r.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.kind() == cc::rec::event_kind::sample)
                    best = cc::max(best, t.frames_of(e).size());
            });
        return best;
    };

    // Two runs of the same recursion, so the DEPTH is comparable even though the samples are not the same samples.
    // Interning must not cost frames, which a truncating or mis-sized id would.
    REQUIRE(deepest(inline_run, inline_stacks) > 4);
    CHECK(deepest(interned_run, interned_stacks) > 4);
}

REC_TEST("record/stacks - an interned sample carries an id and nothing else")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});
    REQUIRE(count_interned(r) > 0);

    cc::rec::stack_table const stacks(r);

    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::sample || !e.has_interned_stack())
                return;

            // The saving IS this: one entry on the wire where the addresses would have been.
            auto const raw = e.field_as_u64_array("frames");
            CHECK(raw.size() == 1);
            CHECK(raw.front() != 0);
            CHECK(!stacks.frames_of_id(raw.front()).empty());
        });
}

REC_TEST("record/stacks - an unknown id resolves to nothing rather than to something wrong")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording const empty;
    cc::rec::stack_table const stacks(empty);

    // A recording whose definitions were decimated away is the real case, and a confident wrong stack would be far
    // worse than an empty one.
    CHECK(stacks.frames_of_id(1).empty());
    CHECK(stacks.frames_of_id(0).empty());
}

TEST("record/stacks - what interning is worth", nx::config::manual, nx::config::exclusive(), nx::config::owns_recorder)
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const bytes_of = [](cc::rec::recording const& r)
    {
        isize n = 0;
        for (auto const& b : r.blocks())
            n += b.bytes().size();
        return n;
    };

    cc::println("");
    cc::println("  depth   inline B   interned B   ratio   stacks   samples");

    for (auto const depth : {2, 8, 24, 48})
    {
        cc::rec::recording_listener plain;
        {
            scoped_listener const reg(plain);
            {
                cc::rec::sampling_scope const s({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 0});
                burn_at_depth(depth, 0.25);
            }
            cc::rec::flush_blocking();
        }
        auto const a = plain.take();

        cc::rec::recording_listener interned;
        {
            scoped_listener const reg(interned);
            {
                cc::rec::sampling_scope const s({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});
                burn_at_depth(depth, 0.25);
            }
            cc::rec::flush_blocking();
        }
        auto const b = interned.take();

        auto const ia = bytes_of(a);
        auto const ib = bytes_of(b);
        cc::rec::stack_table const t(b);

        cc::println("  {:5}   {:8}   {:10}   {:5.2f}   {:6}   {:7}", depth, ia, ib, ia > 0 ? f64(ib) / f64(ia) : 0.0,
                    t.size(), b.count_of_kind(cc::rec::event_kind::sample));
    }
}

REC_TEST("record/stacks - interning survives a round trip through bytes")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const original = capture_deep({.rate_hz = 500.0, .stop_at_scope = false, .intern_min_frames = 4});
    REQUIRE(count_interned(original) > 0);

    auto const bytes = cc::rec::serialize(original);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();

    // The flag rides in the event header, and the kind rides in the descriptor table.
    // Losing either turns every interned sample into a stack of one nonsense address, which reads as real data.
    CHECK(count_interned(r) == count_interned(original));
    CHECK(r.count_of_kind(cc::rec::event_kind::stack_definition)
          == original.count_of_kind(cc::rec::event_kind::stack_definition));

    cc::rec::stack_table const stacks(r);
    CHECK(stacks.size() == cc::rec::stack_table(original).size());
    CHECK(stacks.total_frames() == cc::rec::stack_table(original).total_frames());

    isize resolved = 0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::sample && e.has_interned_stack() && !stacks.frames_of(e).empty())
                ++resolved;
        });
    CHECK(resolved == count_interned(r));
}
