#include <nexus/test.hh>
#include <shaped-viewer/resources/impl/range_allocator.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the allocator the central geometry pools sub-allocate meshes from.

TEST("sv - range_allocator hands out disjoint ranges first fit")
{
    auto a = sv::impl::range_allocator(10);
    CHECK(a.allocate(4) == isize(0));
    CHECK(a.allocate(3) == isize(4));
    CHECK(a.used() == 7);
    CHECK(a.largest_free() == 3);
    CHECK(!a.allocate(4).has_value()); // only three left
    CHECK(a.allocate(3) == isize(7));
    CHECK(a.largest_free() == 0);
}

TEST("sv - range_allocator coalesces freed neighbors")
{
    auto a = sv::impl::range_allocator(9);
    auto const x = a.allocate(3).value();
    auto const y = a.allocate(3).value();
    auto const z = a.allocate(3).value();

    a.free(x, 3);
    a.free(z, 3);
    CHECK(a.largest_free() == 3); // two separate holes around y

    // Freeing the middle merges all three into one range the size of the span.
    a.free(y, 3);
    CHECK(a.used() == 0);
    CHECK(a.largest_free() == 9);
    CHECK(a.allocate(9) == isize(0));
}

TEST("sv - range_allocator reuses a hole before the tail")
{
    auto a = sv::impl::range_allocator(8);
    auto const x = a.allocate(2).value();
    (void)a.allocate(2);
    a.free(x, 2);
    CHECK(a.allocate(2) == x);
}

TEST("sv - range_allocator grows its tail")
{
    auto a = sv::impl::range_allocator(4);
    (void)a.allocate(3);
    CHECK(!a.allocate(4).has_value());

    // The free element at the end joins the grown tail, so a range can straddle the old capacity.
    a.grow(8);
    CHECK(a.capacity() == 8);
    CHECK(a.largest_free() == 5);
    CHECK(a.allocate(4) == isize(3));
}

TEST("sv - range_allocator grows past a full span")
{
    auto a = sv::impl::range_allocator();
    CHECK(!a.allocate(1).has_value());
    a.grow(2);
    CHECK(a.allocate(2) == isize(0));
    a.grow(5);
    CHECK(a.allocate(3) == isize(2));
}
