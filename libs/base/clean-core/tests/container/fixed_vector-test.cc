#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

namespace
{
// Tracks ctor/dtor balance so we can assert elements are constructed / destroyed exactly once.
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

TEST("fixed_vector - empty")
{
    cc::fixed_vector<int, 4> v;
    CHECK(v.size() == 0);
    CHECK(v.empty());
    CHECK(!v.full());
    CHECK(v.capacity() == 4);
    CHECK((cc::fixed_vector<int, 4>::capacity() == 4));
    CHECK(v.capacity_back() == 4);
    CHECK(v.begin() == v.end());
}

TEST("fixed_vector - fill to capacity")
{
    cc::fixed_vector<int, 4> v;
    v.push_back(10);
    v.push_back(20);
    v.emplace_back(30);
    v.push_back(40);

    CHECK(v.size() == 4);
    CHECK(v.full());
    CHECK(v.capacity_back() == 0);
    CHECK(v[0] == 10);
    CHECK(v[3] == 40);
    CHECK(v.front() == 10);
    CHECK(v.back() == 40);

    int sum = 0;
    for (int x : v)
        sum += x;
    CHECK(sum == 100);
}

TEST("fixed_vector - initializer_list")
{
    cc::fixed_vector<int, 8> v = {1, 2, 3};
    CHECK(v.size() == 3);
    CHECK(v[0] == 1);
    CHECK(v[2] == 3);
}

TEST("fixed_vector - pop_back / remove_back / clear")
{
    cc::fixed_vector<int, 4> v = {1, 2, 3};
    CHECK(v.pop_back() == 3);
    CHECK(v.size() == 2);
    v.remove_back();
    CHECK(v.size() == 1);
    CHECK(v.back() == 1);
    v.clear();
    CHECK(v.empty());
}

TEST("fixed_vector - copy and move have value semantics")
{
    cc::fixed_vector<int, 4> a = {1, 2, 3};
    cc::fixed_vector<int, 4> b = a; // copy
    CHECK(b.size() == 3);
    b[0] = 99;
    CHECK(a[0] == 1); // deep copy — a is untouched

    cc::fixed_vector<int, 4> c = cc::move(a); // move
    CHECK(c.size() == 3);
    CHECK(c[2] == 3);
    CHECK(a.empty()); // moved-from is empty
}

TEST("fixed_vector - constructs and destroys each element exactly once")
{
    Tracked::alive = 0;
    {
        cc::fixed_vector<Tracked, 8> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.push_back(Tracked(3));
        CHECK(Tracked::alive == 3);

        auto copy = v; // +3
        CHECK(Tracked::alive == 6);

        copy.clear(); // -3
        CHECK(Tracked::alive == 3);

        (void)v.pop_back(); // -1
        CHECK(Tracked::alive == 2);
    } // v destroyed -> -2
    CHECK(Tracked::alive == 0);
}

TEST("fixed_vector - holds move-only types")
{
    cc::fixed_vector<MoveOnly, 4> v;
    v.emplace_back(7);
    v.push_back(MoveOnly(8));
    CHECK(v.size() == 2);
    CHECK(v[0].value == 7);

    cc::fixed_vector<MoveOnly, 4> moved = cc::move(v);
    CHECK(moved.size() == 2);
    CHECK(moved[1].value == 8);
    CHECK(v.empty());
}

TEST("fixed_vector - factories mirror cc::vector")
{
    int const src[] = {1, 2, 3};
    auto copy = cc::fixed_vector<int, 8>::create_copy_of(cc::span<int const>(src));
    CHECK(copy.size() == 3);
    CHECK(copy[2] == 3);

    auto def = cc::fixed_vector<int, 8>::create_defaulted(4);
    CHECK(def.size() == 4);
    CHECK(def[0] == 0);

    auto filled = cc::fixed_vector<int, 8>::create_filled(3, 7);
    CHECK(filled.size() == 3);
    CHECK(filled[1] == 7);
}

TEST("fixed_vector - remove_at preserves order; unordered swaps last")
{
    cc::fixed_vector<int, 8> v = {0, 1, 2, 3, 4};
    v.remove_at(1); // -> 0 2 3 4
    CHECK(v.size() == 4);
    CHECK(v[0] == 0);
    CHECK(v[1] == 2);
    CHECK(v[3] == 4);

    cc::fixed_vector<int, 8> u = {0, 1, 2, 3, 4};
    u.remove_at_unordered(1); // last (4) swaps into slot 1 -> 0 4 2 3
    CHECK(u.size() == 4);
    CHECK(u[1] == 4);
}

TEST("fixed_vector - range removal")
{
    cc::fixed_vector<int, 8> v = {0, 1, 2, 3, 4};
    v.remove_at_range(1, 2); // remove 1,2 -> 0 3 4
    CHECK(v.size() == 3);
    CHECK(v[0] == 0);
    CHECK(v[1] == 3);
    CHECK(v[2] == 4);

    cc::fixed_vector<int, 8> u = {0, 1, 2, 3, 4};
    u.remove_at_range_unordered(1, 2); // fill the gap with tail -> keeps {0,3,4}
    CHECK(u.size() == 3);
    CHECK(u[0] == 0);
    // the two survivors after index 0 are 3 and 4 in some order
    CHECK(((u[1] == 3 && u[2] == 4) || (u[1] == 4 && u[2] == 3)));
}

TEST("fixed_vector - predicate removal and retain")
{
    cc::fixed_vector<int, 8> v = {1, 2, 3, 4, 5, 6};
    auto removed = v.remove_all_where([](int x) { return x % 2 == 0; });
    CHECK(removed == 3);
    CHECK(v.size() == 3);
    CHECK(v[0] == 1);
    CHECK(v[1] == 3);
    CHECK(v[2] == 5);

    cc::fixed_vector<int, 8> u = {1, 2, 2, 3, 2};
    CHECK(u.remove_all_value(2) == 3);
    CHECK(u.size() == 2);

    cc::fixed_vector<int, 8> r = {1, 2, 3, 4};
    r.retain_all_where([](int x) { return x > 2; });
    CHECK(r.size() == 2);
    CHECK(r[0] == 3);
}

TEST("fixed_vector - resize family and fill")
{
    cc::fixed_vector<int, 8> v = {1, 2, 3};
    v.resize_to_filled(5, 9); // grow with 9s -> 1 2 3 9 9
    CHECK(v.size() == 5);
    CHECK(v[4] == 9);
    v.resize_down_to(2); // -> 1 2
    CHECK(v.size() == 2);

    v.fill(0);
    CHECK(v[0] == 0);
    CHECK(v[1] == 0);

    v.clear_resize_to_defaulted(3);
    CHECK(v.size() == 3);
    CHECK(v[2] == 0);
}
