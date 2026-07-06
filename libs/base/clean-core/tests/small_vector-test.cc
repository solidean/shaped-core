#include <clean-core/common/utility.hh>
#include <clean-core/container/small_vector.hh>
#include <nexus/test.hh>

namespace
{
// Tracks ctor/dtor balance so we can assert no elements leak across an inline→heap spill.
struct Tracked
{
    int value = 0;
    static inline int alive = 0;

    Tracked() { ++alive; }
    explicit Tracked(int v) : value(v) { ++alive; }
    Tracked(Tracked const& rhs) : value(rhs.value) { ++alive; }
    Tracked(Tracked&& rhs) noexcept : value(rhs.value) { ++alive; }
    Tracked& operator=(Tracked const&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

struct MoveOnly
{
    int value = 0;
    MoveOnly() = default;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(MoveOnly&& rhs) noexcept : value(rhs.value) { rhs.value = -1; }
    MoveOnly& operator=(MoveOnly&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
};
} // namespace

TEST("small_vector - empty is inline")
{
    cc::small_vector<int, 4> v;
    CHECK(v.size() == 0);
    CHECK(v.empty());
    CHECK(v.is_inline());
    CHECK(v.capacity() == 4);
    CHECK((cc::small_vector<int, 4>::inline_capacity() == 4));
    CHECK(v.begin() == v.end());
}

TEST("small_vector - fill within inline capacity does not allocate")
{
    cc::small_vector<int, 4> v;
    v.push_back(10);
    v.push_back(20);
    v.emplace_back(30);
    v.push_back(40);

    CHECK(v.size() == 4);
    CHECK(v.is_inline()); // still inline at exactly N
    CHECK(v[0] == 10);
    CHECK(v[3] == 40);
    CHECK(v.front() == 10);
    CHECK(v.back() == 40);
}

TEST("small_vector - overflow spills to heap and preserves values")
{
    cc::small_vector<int, 4> v;
    for (int i = 0; i < 100; ++i)
        v.push_back(i * 2);

    CHECK(v.size() == 100);
    CHECK(!v.is_inline()); // spilled
    CHECK(v.capacity() >= 100);
    for (int i = 0; i < 100; ++i)
        CHECK(v[i] == i * 2);
}

TEST("small_vector - emplace_back returns reference")
{
    cc::small_vector<int, 2> v;
    int& r = v.emplace_back(7);
    CHECK(r == 7);
    r = 9;
    CHECK(v[0] == 9);
}

TEST("small_vector - copy is a deep copy")
{
    SECTION("inline")
    {
        cc::small_vector<int, 4> a;
        a.push_back(1);
        a.push_back(2);

        cc::small_vector<int, 4> b(a);
        CHECK(b.size() == 2);
        CHECK(b.is_inline());
        CHECK(b.data() != a.data());
        a[0] = 99;
        CHECK(b[0] == 1);
    }

    SECTION("heap")
    {
        cc::small_vector<int, 2> a;
        for (int i = 0; i < 10; ++i)
            a.push_back(i);

        cc::small_vector<int, 2> b(a);
        CHECK(b.size() == 10);
        CHECK(!b.is_inline());
        CHECK(b.data() != a.data());
        for (int i = 0; i < 10; ++i)
            CHECK(b[i] == i);
    }
}

TEST("small_vector - move")
{
    SECTION("inline source: elements moved, source emptied")
    {
        cc::small_vector<int, 4> a;
        a.push_back(1);
        a.push_back(2);

        cc::small_vector<int, 4> b(cc::move(a));
        CHECK(b.size() == 2);
        CHECK(b[0] == 1);
        CHECK(b[1] == 2);
        CHECK(a.size() == 0);
    }

    SECTION("heap source: buffer stolen, source empty and inline again")
    {
        cc::small_vector<int, 2> a;
        for (int i = 0; i < 10; ++i)
            a.push_back(i);
        auto const* const stolen = a.data();

        cc::small_vector<int, 2> b(cc::move(a));
        CHECK(b.size() == 10);
        CHECK(b.data() == stolen); // no reallocation on steal
        CHECK(a.size() == 0);
        CHECK(a.is_inline());
    }
}

TEST("small_vector - assignment")
{
    SECTION("copy assignment")
    {
        cc::small_vector<int, 2> a;
        for (int i = 0; i < 5; ++i)
            a.push_back(i);
        cc::small_vector<int, 2> b;
        b.push_back(99);

        b = a;
        CHECK(b.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(b[i] == i);

        b = b; // self-assign is a no-op
        CHECK(b.size() == 5);
    }

    SECTION("move assignment")
    {
        cc::small_vector<int, 2> a;
        for (int i = 0; i < 5; ++i)
            a.push_back(i);
        cc::small_vector<int, 2> b;
        b.push_back(99);

        b = cc::move(a);
        CHECK(b.size() == 5);
        CHECK(b[4] == 4);
        CHECK(a.size() == 0);
    }
}

TEST("small_vector - pop_back / remove_back / clear / resize / reserve")
{
    cc::small_vector<int, 4> v;
    for (int i = 0; i < 6; ++i)
        v.push_back(i);

    CHECK(v.pop_back() == 5); // pop_back returns the element (cc::vector-consistent)
    CHECK(v.size() == 5);
    CHECK(v.back() == 4);

    v.remove_back(); // remove_back is the void form
    CHECK(v.size() == 4);
    CHECK(v.back() == 3);

    v.reserve(64);
    CHECK(v.capacity() >= 64);
    CHECK(v.size() == 4);

    v.resize_down_to(2);
    CHECK(v.size() == 2);
    CHECK(v[1] == 1);

    v.resize_to_defaulted(4);
    CHECK(v.size() == 4);
    CHECK(v[2] == 0); // value-initialized
    CHECK(v[3] == 0);

    v.resize_to_filled(6, 9);
    CHECK(v.size() == 6);
    CHECK(v[4] == 9);
    CHECK(v[5] == 9);

    v.clear();
    CHECK(v.empty());
}

TEST("small_vector - factories mirror cc::vector")
{
    auto d = cc::small_vector<int, 4>::create_defaulted(3);
    CHECK(d.size() == 3);
    CHECK(d[0] == 0);
    CHECK(d.is_inline());

    auto f = cc::small_vector<int, 2>::create_filled(5, 7); // > N -> heap
    CHECK(f.size() == 5);
    CHECK(!f.is_inline());
    CHECK(f[4] == 7);

    int const src[3] = {1, 2, 3};
    auto c = cc::small_vector<int, 4>::create_copy_of(cc::span<int const>(src, 3));
    CHECK(c.size() == 3);
    CHECK(c[2] == 3);

    auto cap = cc::small_vector<int, 2>::create_with_capacity(32);
    CHECK(cap.empty());
    CHECK(cap.capacity() >= 32);
    CHECK(!cap.is_inline());
}

TEST("small_vector - extract_allocation and try_extract_allocation")
{
    SECTION("inline is materialized on extract")
    {
        cc::small_vector<int, 4> v;
        v.push_back(1);
        v.push_back(2);
        CHECK(v.is_inline());

        auto alloc = v.extract_allocation(); // must materialize the inline elements to the heap
        CHECK(alloc.obj_end - alloc.obj_start == 2);
        CHECK(alloc.obj_start[0] == 1);
        CHECK(alloc.obj_start[1] == 2);
        CHECK(v.empty());
        CHECK(v.is_inline());
    }

    SECTION("try_extract only succeeds in heap mode")
    {
        cc::small_vector<int, 2> inl;
        inl.push_back(1);
        CHECK(!inl.try_extract_allocation().has_value()); // inline -> nullopt, still holds its element
        CHECK(inl.size() == 1);

        cc::small_vector<int, 2> heap;
        for (int i = 0; i < 5; ++i)
            heap.push_back(i);
        CHECK(!heap.is_inline());
        auto taken = heap.try_extract_allocation();
        REQUIRE(taken.has_value());
        CHECK(taken.value().obj_end - taken.value().obj_start == 5);
        CHECK(heap.empty());
    }

    SECTION("round-trip through create_from_allocation")
    {
        cc::small_vector<int, 4> v;
        v.push_back(10);
        v.push_back(20);
        auto v2 = cc::small_vector<int, 4>::create_from_allocation(v.extract_allocation());
        CHECK(!v2.is_inline()); // adopting an allocation is always heap mode
        CHECK(v2.size() == 2);
        CHECK(v2[0] == 10);
        CHECK(v2[1] == 20);
    }
}

TEST("small_vector - element lifetimes balance across a spill")
{
    Tracked::alive = 0;
    {
        cc::small_vector<Tracked, 2> v;
        for (int i = 0; i < 20; ++i) // forces at least one inline→heap reallocation
            v.emplace_back(i);
        CHECK(v.size() == 20);
        CHECK(Tracked::alive == 20);
        CHECK(v[19].value == 19);

        auto w = v; // deep copy
        CHECK(Tracked::alive == 40);
        auto z = cc::move(w); // move: no new live objects
        CHECK(Tracked::alive == 40);
        CHECK(z[0].value == 0);
    }
    CHECK(Tracked::alive == 0);
}

TEST("small_vector - move-only element type")
{
    cc::small_vector<MoveOnly, 2> v;
    v.emplace_back(1);
    v.emplace_back(2);
    v.emplace_back(3); // spill to heap, move-constructing the inline elements over

    CHECK(v.size() == 3);
    CHECK(v[0].value == 1);
    CHECK(v[2].value == 3);

    cc::small_vector<MoveOnly, 2> w(cc::move(v));
    CHECK(w.size() == 3);
    CHECK(w[1].value == 2);
    CHECK(v.size() == 0);
}

TEST("small_vector - initializer list and hash")
{
    cc::small_vector<int, 4> v = {1, 2, 3, 4, 5}; // > N -> heap
    CHECK(v.size() == 5);
    CHECK(v[0] == 1);
    CHECK(v[4] == 5);

    cc::small_vector<int, 8> const w = {1, 2, 3, 4, 5}; // inline, same content
    CHECK(hash(v) == hash(w));                          // hash is content-based, independent of storage mode
}

TEST("small_vector - stable appenders require capacity")
{
    cc::small_vector<int, 2> v;
    v.reserve(8);
    auto const* const data0 = v.data();
    v.push_back_stable(1);
    v.emplace_back_stable(2);
    CHECK(v.data() == data0); // no reallocation
    CHECK(v.size() == 2);
    CHECK(v[1] == 2);

    cc::small_vector<int, 1> full;
    full.push_back(1);                          // size == capacity (1)
    CHECK_ASSERTS(full.emplace_back_stable(3)); // no spare capacity -> asserts
}

TEST("small_vector - ordered and unordered element removal")
{
    auto make = []
    {
        cc::small_vector<int, 3> v;
        for (int i = 0; i < 6; ++i) // 0..5, heap
            v.push_back(i);
        return v;
    };

    SECTION("remove_at / pop_at preserve order")
    {
        auto v = make();
        CHECK(v.pop_at(1) == 1);
        CHECK(v.size() == 5);
        CHECK(v[0] == 0);
        CHECK(v[1] == 2); // shifted down
        v.remove_at(0);
        CHECK(v[0] == 2);
    }

    SECTION("unordered swaps in the last element")
    {
        auto v = make();
        CHECK(v.pop_at_unordered(0) == 0);
        CHECK(v.size() == 5);
        CHECK(v[0] == 5); // last moved into the hole
        v.remove_at_unordered(1);
        CHECK(v.size() == 4);
    }

    SECTION("range removal")
    {
        auto v = make();
        v.remove_at_range(1, 2); // drop 1,2
        CHECK(v.size() == 4);
        CHECK(v[0] == 0);
        CHECK(v[1] == 3);
        v.remove_from_to(0, 1); // drop 0
        CHECK(v[0] == 3);
    }

    SECTION("predicate and value removal")
    {
        auto v = make();
        CHECK(v.remove_all_where([](int x) { return x % 2 == 0; }) == 3); // drop 0,2,4
        CHECK(v.size() == 3);
        CHECK(v[0] == 1);

        auto w = make();                                             // {0,1,2,3,4,5}
        CHECK(w.remove_first_value(3).value() == 3);                 // -> {0,1,2,4,5}
        CHECK(w.retain_all_where([](int x) { return x < 3; }) == 2); // drop 4,5 -> keep {0,1,2}
        CHECK(w.size() == 3);
    }
}

TEST("small_vector - resize_to_constructed and clear_resize")
{
    cc::small_vector<int, 2> v;
    v.resize_to_constructed(4, 7);
    CHECK(v.size() == 4);
    CHECK(v[0] == 7);
    CHECK(v[3] == 7);

    v.clear_resize_to_filled(3, 9);
    CHECK(v.size() == 3);
    CHECK(v[2] == 9);

    v.clear_resize_to_defaulted(2);
    CHECK(v[0] == 0);
}

TEST("small_vector - shrink_to_fit returns to inline when it fits")
{
    cc::small_vector<int, 4> v;
    for (int i = 0; i < 20; ++i)
        v.push_back(i);
    CHECK(!v.is_inline());

    v.resize_down_to(3); // now fits inline, but still heap
    CHECK(!v.is_inline());
    v.shrink_to_fit();
    CHECK(v.is_inline()); // re-inlined
    CHECK(v.size() == 3);
    CHECK(v[0] == 0);
    CHECK(v[2] == 2);
}
