#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Packing: the size field uses the smallest unsigned type that fits N and fills the tail padding next to
// the aligned storage — u8 for fixed_vector<u8,10>, u16 for fixed_vector<u8,300>, u64 for fixed_vector<u64,2>.
static_assert(sizeof(cc::fixed_vector<u8, 10>) == 11, "u8/10 -> 1-byte size field, no padding");
static_assert(sizeof(cc::fixed_vector<u8, 300>) == 302, "u8/300 -> u16 size field");
static_assert(sizeof(cc::fixed_vector<u64, 2>) == 24, "u64/2 -> u64 size field fills the 8-byte padding");

namespace
{
// Tracks ctor/dtor balance so we can assert elements are constructed / destroyed exactly once.
struct Tracked
{
    int value = 0;
    static inline thread_local int alive = 0;

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

TEST("fixed_vector - N == 0 is a valid permanently-empty vector")
{
    cc::fixed_vector<int, 0> v;
    CHECK(v.size() == 0);
    CHECK(v.empty());
    CHECK(v.full()); // 0 == capacity
    CHECK(v.capacity() == 0);
    CHECK(v.capacity_back() == 0);
    CHECK(v.begin() == v.end());

    auto copy = v; // copy / move of an empty N==0 vector is well-formed
    CHECK(copy.empty());
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
    v.remove_at_range({.offset = 1, .size = 2}); // remove 1,2 -> 0 3 4
    CHECK(v.size() == 3);
    CHECK(v[0] == 0);
    CHECK(v[1] == 3);
    CHECK(v[2] == 4);

    cc::fixed_vector<int, 8> u = {0, 1, 2, 3, 4};
    u.remove_at_range_unordered({.offset = 1, .size = 2}); // fill the gap with tail -> keeps {0,3,4}
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

TEST("fixed_vector - push_back_range")
{
    cc::fixed_vector<int, 8> v;
    v.push_back(0);

    cc::fixed_vector<int, 4> src;
    src.push_back(1);
    src.push_back(2);

    v.push_back_range(src);

    REQUIRE(v.size() == 3);
    CHECK(v[1] == 1);
    CHECK(v[2] == 2);
}

TEST("fixed_vector - insert_at / emplace_at")
{
    SECTION("insert into middle, front and at size()")
    {
        cc::fixed_vector<int, 8> v;
        for (int i = 0; i < 3; ++i)
            v.push_back(i); // 0 1 2

        v.insert_at(1, 9);        // -> 0 9 1 2
        v.insert_at(0, 8);        // -> 8 0 9 1 2
        v.insert_at(v.size(), 7); // -> 8 0 9 1 2 7

        REQUIRE(v.size() == 6);
        CHECK(v[0] == 8);
        CHECK(v[1] == 0);
        CHECK(v[2] == 9);
        CHECK(v[3] == 1);
        CHECK(v[4] == 2);
        CHECK(v[5] == 7);
    }

    SECTION("move-only elements")
    {
        cc::fixed_vector<MoveOnly, 4> v;
        v.emplace_back(1);
        v.emplace_back(3);

        v.emplace_at(1, 2);

        REQUIRE(v.size() == 3);
        CHECK(v[0].value == 1);
        CHECK(v[1].value == 2);
        CHECK(v[2].value == 3);
    }

    SECTION("overflowing the capacity asserts")
    {
        cc::fixed_vector<int, 2> v;
        v.push_back(0);
        v.push_back(1);

        CHECK_ASSERTS(v.insert_at(0, 2));
    }
}

TEST("fixed_vector - replace_range")
{
    SECTION("longer, shorter and equal replacements")
    {
        cc::fixed_vector<int, 8> v;
        for (int i = 0; i < 4; ++i)
            v.push_back(i); // 0 1 2 3

        cc::fixed_vector<int, 8> src;
        src.push_back(7);
        src.push_back(8);
        src.push_back(9);

        v.replace_range({.offset = 1, .size = 1}, src); // -> 0 7 8 9 2 3
        REQUIRE(v.size() == 6);
        CHECK(v[1] == 7);
        CHECK(v[3] == 9);
        CHECK(v[4] == 2);

        v.replace_range({.offset = 1, .size = 3}, cc::fixed_vector<int, 1>()); // -> 0 2 3
        REQUIRE(v.size() == 3);
        CHECK(v[0] == 0);
        CHECK(v[1] == 2);
        CHECK(v[2] == 3);
    }

    // The replaced run frees room, so a full vector still takes an equal-sized replacement.
    SECTION("equal-sized replacement on a full vector")
    {
        cc::fixed_vector<int, 4> v;
        for (int i = 0; i < 4; ++i)
            v.push_back(i);

        cc::fixed_vector<int, 4> src;
        for (int i = 0; i < 4; ++i)
            src.push_back(10 + i);

        v.replace_range({.offset = 0, .size = 4}, src);

        REQUIRE(v.size() == 4);
        CHECK(v[0] == 10);
        CHECK(v[3] == 13);
    }

    SECTION("a replacement that would overflow asserts")
    {
        cc::fixed_vector<int, 4> v;
        for (int i = 0; i < 4; ++i)
            v.push_back(i);

        cc::fixed_vector<int, 4> src;
        src.push_back(1);
        src.push_back(2);

        CHECK_ASSERTS(v.replace_range({.offset = 0, .size = 1}, src));
    }
}

TEST("fixed_vector - replace_range object lifetimes")
{
    for (int old_size = 0; old_size <= 4; ++old_size)
        for (int start = 0; start <= old_size; ++start)
            for (int count = 0; count <= old_size - start; ++count)
                for (int new_count = 0; new_count <= 4; ++new_count)
                {
                    Tracked::alive = 0;
                    {
                        cc::fixed_vector<Tracked, 8> v;
                        for (int i = 0; i < old_size; ++i)
                            v.push_back(Tracked(i));

                        cc::fixed_vector<Tracked, 8> src;
                        for (int i = 0; i < new_count; ++i)
                            src.push_back(Tracked(100 + i));

                        v.replace_range({.offset = start, .size = count}, src);

                        REQUIRE(v.size() == old_size - count + new_count);
                        for (int i = 0; i < start; ++i)
                            CHECK(v[i].value == i);
                        for (int i = 0; i < new_count; ++i)
                            CHECK(v[start + i].value == 100 + i);
                        for (int i = 0; i < old_size - start - count; ++i)
                            CHECK(v[start + new_count + i].value == start + count + i);
                    }
                    CHECK(Tracked::alive == 0);
                }
}
