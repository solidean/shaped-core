#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/resources/impl/slot_table.hh>

using namespace cc::primitive_defines;

// sv::impl::slot_table — one bindless category's key → element-index identity map.
// Pure CPU: the table holds no views, so no device is involved; the owner mirrors mints and reclaims onto
// the staging group through acquire's `inserted` result and `on_reclaimed` hook, which is what these pin.

namespace
{
[[nodiscard]] sg::epoch ep(u64 e)
{
    return sg::epoch(u64(sg::epoch::first) + e);
}

// An on_reclaimed hook that must never fire.
constexpr auto no_reclaim = [](u32) { CHECK(false); };
} // namespace

TEST("sv slot_table - a re-acquired key keeps its slot and mints nothing")
{
    auto table = sv::impl::slot_table(4);

    auto const s0 = table.acquire(1, ep(0), no_reclaim);
    auto const s1 = table.acquire(2, ep(0), no_reclaim);
    CHECK(s0.inserted);
    CHECK(s1.inserted);
    CHECK(s0.index != s1.index);

    // The same keys in a later epoch: same slots, no mint — the owner writes no descriptor.
    auto const s0_again = table.acquire(1, ep(1), no_reclaim);
    auto const s1_again = table.acquire(2, ep(1), no_reclaim);
    CHECK(!s0_again.inserted);
    CHECK(!s1_again.inserted);
    CHECK(s0_again.index == s0.index);
    CHECK(s1_again.index == s1.index);

    // A new key mints again.
    auto const s2 = table.acquire(3, ep(1), no_reclaim);
    CHECK(s2.inserted);
    CHECK(s2.index != s0.index);
    CHECK(s2.index != s1.index);
    CHECK(table.occupied_count() == 3);
}

TEST("sv slot_table - a full table clears every stale slot at once")
{
    auto table = sv::impl::slot_table(3);
    (void)table.acquire(1, ep(0), no_reclaim);
    (void)table.acquire(2, ep(0), no_reclaim);
    auto const s2 = table.acquire(3, ep(1), no_reclaim);

    // The epoch-1 mint finds the table full: keys 1 and 2 (last acquired in epoch 0) are BOTH reclaimed —
    // the owner hears about each one — while key 3, acquired this epoch, survives in place.
    auto reclaimed = cc::vector<u32>();
    auto const s3 = table.acquire(4, ep(1), [&](u32 freed) { reclaimed.push_back(freed); });
    CHECK(s3.inserted);
    CHECK(reclaimed.size() == 2);
    CHECK(s3.index != s2.index);
    CHECK(table.acquire(3, ep(1), no_reclaim).index == s2.index);

    // Both stale keys are gone, so key 1 mints afresh into the second freed slot.
    auto const s1_again = table.acquire(1, ep(1), no_reclaim);
    CHECK(s1_again.inserted);
    CHECK(s1_again.index != s2.index);
    CHECK(s1_again.index != s3.index);
    CHECK(table.occupied_count() == 3);

    // Key 2 would need a fourth slot, and every slot is now current-epoch: the working set exceeds capacity.
    CHECK_ASSERTS(table.acquire(2, ep(1), no_reclaim));
}

TEST("sv slot_table - a slot acquired this epoch is never reclaimed")
{
    auto table = sv::impl::slot_table(2);
    (void)table.acquire(1, ep(0), no_reclaim);
    (void)table.acquire(2, ep(1), no_reclaim);

    // Key 1 is stale, so the epoch-1 mint may only take key 1's slot — key 2 was acquired this epoch.
    auto const s2 = table.acquire(2, ep(1), no_reclaim);
    auto reclaimed = cc::vector<u32>();
    auto const s3 = table.acquire(3, ep(1), [&](u32 freed) { reclaimed.push_back(freed); });
    CHECK(reclaimed.size() == 1);
    CHECK(s3.index != s2.index);
}

TEST("sv slot_table - a full table of current-epoch slots asserts")
{
    auto table = sv::impl::slot_table(2);
    (void)table.acquire(1, ep(0), no_reclaim);
    (void)table.acquire(2, ep(0), no_reclaim);

    // Both slots were acquired in this epoch: the working set exceeds the capacity.
    CHECK_ASSERTS(table.acquire(3, ep(0), [](u32) {}));
}
