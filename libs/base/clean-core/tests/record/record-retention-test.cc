#include "record-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Retention is what makes a capture that runs for hours possible, and the reason there are two knobs rather than one
// is that the two shapes people ask for disagree about which limit yields.
//
// These are built from synthetic blocks rather than from a live capture: a policy is a decision about which blocks to
// keep, and asserting it against real timing would test the machine instead.

namespace
{
/// A block that claims a size and a wall time, which is everything a policy looks at.
///
/// `owned` rather than a chunk reference, because a synthesized block is exactly what the algebra already handles for
/// filtering and decimation — no recorder has to be running for this to be a real recording.
[[nodiscard]] cc::rec::recorded_block block_at(f64 wall_secs, isize bytes)
{
    cc::rec::recorded_block b;
    cc::vector<byte> payload;
    payload.reserve(bytes);
    for (isize i = 0; i < bytes; ++i)
        payload.push_back(byte(0));

    b.owned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
    b.from = 0;
    b.to = u32(bytes);
    b.base_wall_secs = wall_secs;
    b.seal_wall_secs = wall_secs;
    b.thread_index = 0;
    return b;
}

/// A recording covering `count` seconds, one block per second, each `bytes` big.
/// Block i is at wall time i, so block 0 is the oldest and the newest is at `count - 1`.
[[nodiscard]] cc::rec::recording timeline(isize count, isize bytes)
{
    cc::rec::recording r;
    for (isize i = 0; i < count; ++i)
        r.append_block(block_at(f64(i), bytes));
    return r;
}

[[nodiscard]] f64 oldest_wall_secs(cc::rec::recording const& r)
{
    auto oldest = 1e18;
    for (auto const& b : r.blocks())
        oldest = cc::min(oldest, b.seal_wall_secs);
    return r.blocks().empty() ? 0.0 : oldest;
}
} // namespace

TEST("record/retention - a default policy keeps everything")
{
    auto r = timeline(100, 1024);

    // The default has to be free and total, because an ordinary scoped capture uses the same listener.
    CHECK(cc::rec::retention_policy{}.keeps_everything());
    CHECK(r.trim({}) == 0);
    CHECK(r.block_count() == 100);
    CHECK(r.total_bytes() == 100 * 1024);
}

TEST("record/retention - an age limit drops what is older and nothing else")
{
    auto r = timeline(100, 1024); // newest is at t = 99

    CHECK(r.trim({.max_secs = 10}) == 89);
    CHECK(r.block_count() == 11); // t = 89..99 inclusive
    CHECK(oldest_wall_secs(r) == 89.0);

    // Trimming again with the same policy is a no-op, so a listener can apply it on every chunk.
    CHECK(r.trim({.max_secs = 10}) == 0);
}

TEST("record/retention - a byte cap evicts oldest-first until it fits")
{
    auto r = timeline(100, 1024);

    CHECK(r.trim({.max_bytes = 10 * 1024}) == 90);
    CHECK(r.total_bytes() <= 10 * 1024);

    // Oldest-first is the only order that leaves a usable window rather than a sieve.
    CHECK(oldest_wall_secs(r) == 90.0);
}

TEST("record/retention - the last 30s, and never more than a byte cap")
{
    // The first shape: both are caps, and whichever binds first wins.
    auto const policy = cc::rec::retention_policy{.max_secs = 30, .max_bytes = 10 * 1024};

    // A 30 s window over one-second blocks holds 31 of them, both ends included.
    auto small = timeline(100, 16);
    CHECK(small.trim(policy) == 69); // the age limit binds; 31 blocks of 16 B are nowhere near the cap
    CHECK(small.block_count() == 31);

    auto large = timeline(100, 4096);
    large.trim(policy);
    CHECK(large.total_bytes() <= 10 * 1024); // the byte cap binds, and it is HARD
    CHECK(large.block_count() < 31);
}

TEST("record/retention - the last 20s whatever it costs, then a cap for the rest")
{
    // The second shape: the recent past is promised, so the cap is what gives.
    // That is the difference from the first shape, and the reason both exist.
    auto const policy = cc::rec::retention_policy{.guaranteed_secs = 20, .max_bytes = 10 * 1024};

    auto r = timeline(100, 4096); // the promised window is 21 blocks, 84 KB, eight times the cap
    r.trim(policy);

    CHECK(r.block_count() == 21);
    CHECK(oldest_wall_secs(r) == 79.0);

    // The promise is kept and the cap is knowingly breached, which is what "whatever it costs" means.
    CHECK(r.total_bytes() > 10 * 1024);
}

TEST("record/retention - a hard age limit outranks the guarantee")
{
    // Contradictory on purpose: 60s promised, 10s allowed.
    // The age limit is the stricter promise, so it wins — otherwise a guarantee would silently disable a cap set
    // right next to it.
    auto r = timeline(100, 1024);
    r.trim({.guaranteed_secs = 60, .max_secs = 10, .max_bytes = 1024});

    CHECK(r.block_count() == 11);
    CHECK(oldest_wall_secs(r) == 89.0);
}

TEST("record/retention - retained leaves the original alone")
{
    auto const r = timeline(100, 1024);
    auto const bounded = r.retained({.max_secs = 5});

    CHECK(r.block_count() == 100); // a recording is a VALUE, so applying a policy yields a new one
    CHECK(bounded.block_count() == 6);
}

TEST("record/retention - a trimmed recording still replays in order")
{
    auto r = timeline(100, 1024);
    r.trim({.max_secs = 10});

    // Eviction must not reorder what survives, or every consumer that assumes stream order silently breaks.
    auto previous = -1.0;
    for (auto const& b : r.blocks())
    {
        CHECK(b.seal_wall_secs > previous);
        previous = b.seal_wall_secs;
    }
}

TEST("record/retention - an empty recording survives any policy")
{
    cc::rec::recording r;
    CHECK(r.trim({.guaranteed_secs = 20, .max_secs = 30, .max_bytes = 1}) == 0);
    CHECK(r.block_count() == 0);
    CHECK(r.total_bytes() == 0);
}

REC_TEST("record/retention - a bounded listener never holds more than its policy allows")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener bounded({.max_bytes = 64 * 1024});
    {
        scoped_listener const reg(bounded);
        for (isize i = 0; i < 20000; ++i)
            CC_RECORD("retention-fill", i);

        cc::rec::flush_blocking();
    }

    // A block is what a chunk reference keeps alive, so the bound is block-granular and one block may overshoot it.
    // What must hold is that this does not grow without limit, which is the whole reason a bounded capture exists.
    auto const held = bounded.result().total_bytes();
    CHECK(held <= 64 * 1024 + deterministic_config().chunk_bytes);
    CHECK(bounded.dropped_blocks() >= 0);
}
