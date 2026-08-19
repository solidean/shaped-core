#include <nexus/test.hh>
#include <shaped-viewer/resources/impl/slot_table.hh>

using namespace cc::primitive_defines;

// sv::impl::slot_table — one bindless category's fixed-capacity CPU mirror.
// Pure CPU: the views under test are null-handle raw views, so no device is involved.

namespace
{
// A distinct raw view per key; the table never dereferences it, so a null-handle view is fine.
[[nodiscard]] sg::raw_view some_view()
{
    return sg::raw_buffer_view{.access = sg::view_class::readonly, .shape = sg::view_shape::raw, .buffer = nullptr};
}

[[nodiscard]] sg::epoch ep(u64 e)
{
    return sg::epoch(u64(sg::epoch::first) + e);
}
} // namespace

TEST("sv slot_table - a re-acquired key keeps its slot and stays clean")
{
    auto table = sv::impl::slot_table(4);
    CHECK(!table.dirty());

    auto const s0 = table.acquire(1, some_view(), ep(0));
    auto const s1 = table.acquire(2, some_view(), ep(0));
    CHECK(s0 != s1);
    CHECK(table.dirty()); // minting changed the mirror
    table.clear_dirty();

    // The same keys in a later epoch: same slots, no mirror change.
    CHECK(table.acquire(1, some_view(), ep(1)) == s0);
    CHECK(table.acquire(2, some_view(), ep(1)) == s1);
    CHECK(!table.dirty());

    // A new key dirties again.
    auto const s2 = table.acquire(3, some_view(), ep(1));
    CHECK(s2 != s0);
    CHECK(s2 != s1);
    CHECK(table.dirty());
    CHECK(table.occupied_count() == 3);
}

TEST("sv slot_table - a full table reclaims the least-recently-acquired slot")
{
    auto table = sv::impl::slot_table(2);
    auto const s0 = table.acquire(1, some_view(), ep(0));
    auto const s1 = table.acquire(2, some_view(), ep(0));

    // Epoch 1 touches only key 2, so key 1's slot is the LRU victim for the new key in epoch 2.
    CHECK(table.acquire(2, some_view(), ep(1)) == s1);
    table.clear_dirty();
    auto const s3 = table.acquire(3, some_view(), ep(2));
    CHECK(s3 == s0);
    CHECK(table.dirty());

    // The reclaimed key is gone: re-acquiring it mints again (evicting key 2, the older of the two now).
    auto const s1_again = table.acquire(1, some_view(), ep(3));
    CHECK(s1_again == s1);
    CHECK(table.occupied_count() == 2);
}

TEST("sv slot_table - a slot acquired this epoch is never reclaimed")
{
    auto table = sv::impl::slot_table(2);
    (void)table.acquire(1, some_view(), ep(0));
    (void)table.acquire(2, some_view(), ep(1));

    // Key 1 is older, so the epoch-1 mint may only take key 1's slot — key 2 was acquired in the current epoch.
    auto const s2 = table.acquire(2, some_view(), ep(1));
    auto const s3 = table.acquire(3, some_view(), ep(1));
    CHECK(s3 != s2);
}

TEST("sv slot_table - a full table of current-epoch slots asserts")
{
    auto table = sv::impl::slot_table(2);
    (void)table.acquire(1, some_view(), ep(0));
    (void)table.acquire(2, some_view(), ep(0));

    // Both slots were acquired in this epoch: the working set exceeds the capacity.
    CHECK_ASSERTS(table.acquire(3, some_view(), ep(0)));
}
