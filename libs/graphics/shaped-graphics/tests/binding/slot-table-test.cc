#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/impl/slot_table.hh>

using namespace cc::primitive_defines;

// sg::impl::slot_table — one bindless array's key → element-index identity map, here over plain u64 keys
// (bindless_array instantiates it over sg::raw_view).
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

TEST("sg slot_table - a re-acquired key keeps its slot and mints nothing")
{
    auto table = sg::impl::slot_table<u64>(4);

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

TEST("sg slot_table - a full table clears every stale slot at once")
{
    auto table = sg::impl::slot_table<u64>(3);
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

TEST("sg slot_table - a slot acquired this epoch is never reclaimed")
{
    auto table = sg::impl::slot_table<u64>(2);
    (void)table.acquire(1, ep(0), no_reclaim);
    (void)table.acquire(2, ep(1), no_reclaim);

    // Key 1 is stale, so the epoch-1 mint may only take key 1's slot — key 2 was acquired this epoch.
    auto const s2 = table.acquire(2, ep(1), no_reclaim);
    auto reclaimed = cc::vector<u32>();
    auto const s3 = table.acquire(3, ep(1), [&](u32 freed) { reclaimed.push_back(freed); });
    CHECK(reclaimed.size() == 1);
    CHECK(s3.index != s2.index);
}

TEST("sg slot_table - a full table of current-epoch slots asserts")
{
    auto table = sg::impl::slot_table<u64>(2);
    (void)table.acquire(1, ep(0), no_reclaim);
    (void)table.acquire(2, ep(0), no_reclaim);

    // Both slots were acquired in this epoch: the working set exceeds the capacity.
    CHECK_ASSERTS(table.acquire(3, ep(0), [](u32) {}));
}

TEST("sg slot_table - a released key frees its slot now, or in the next epoch if this one used it")
{
    auto table = sg::impl::slot_table<u64>(8);
    auto const old = table.acquire(1, ep(0), no_reclaim);
    auto const current = table.acquire(2, ep(1), no_reclaim);

    // Key 1 was last acquired in an earlier epoch, so nothing recorded this epoch can name it: freed at once.
    auto reclaimed = cc::vector<u32>();
    auto const record = [&](u32 freed) { reclaimed.push_back(freed); };
    table.release(1, ep(1), record);
    REQUIRE(reclaimed.size() == 1);
    CHECK(reclaimed[0] == old.index);

    // Key 2 was acquired this epoch, so its index may be in this epoch's work: it waits for the next epoch.
    table.release(2, ep(1), record);
    CHECK(reclaimed.size() == 1);
    (void)table.acquire(3, ep(2), record);
    REQUIRE(reclaimed.size() == 2);
    CHECK(reclaimed[1] == current.index);

    // A pinned key is unpin's to free, not release's.
    auto const pinned = table.acquire_pinned(4, ep(2), record);
    table.release(4, ep(3), record);
    CHECK(table.is_pinned(pinned.index));
}

TEST("sg slot_table - a key acquired again in the epoch it was released is freed with it, and minted afresh")
{
    auto table = sg::impl::slot_table<u64>(8);
    auto reclaimed = cc::vector<u32>();
    auto const record = [&](u32 freed) { reclaimed.push_back(freed); };

    auto const first = table.acquire(1, ep(0), record);
    table.release(1, ep(0), record);
    // Released, then named again in the same epoch: the same slot, and still deferred.
    auto const again = table.acquire(1, ep(0), record);
    CHECK(!again.inserted);
    CHECK(again.index == first.index);
    CHECK(reclaimed.empty());

    // The next epoch's first acquire frees the deferred slot before anything else, so key 1 is a new mint.
    (void)table.acquire(2, ep(1), record);
    REQUIRE(reclaimed.size() == 1);
    CHECK(reclaimed[0] == first.index);
    CHECK(table.acquire(1, ep(1), record).inserted);
}

TEST("sg slot_table - releasing a key twice frees its slot once")
{
    auto table = sg::impl::slot_table<u64>(8);
    auto reclaimed = cc::vector<u32>();
    auto const record = [&](u32 freed) { reclaimed.push_back(freed); };

    // Freed at once: a second release finds no slot.
    (void)table.acquire(1, ep(0), record);
    table.release(1, ep(1), record);
    table.release(1, ep(1), record);
    CHECK(reclaimed.size() == 1);

    // Deferred: the key is listed twice, and its slot is still freed once.
    (void)table.acquire(2, ep(1), record);
    table.release(2, ep(1), record);
    table.release(2, ep(1), record);
    (void)table.acquire(3, ep(2), record);
    CHECK(reclaimed.size() == 2);
}
