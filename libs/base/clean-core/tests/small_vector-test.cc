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

TEST("small_vector - pop_back / clear / resize / reserve")
{
    cc::small_vector<int, 4> v;
    for (int i = 0; i < 6; ++i)
        v.push_back(i);

    v.pop_back();
    CHECK(v.size() == 5);
    CHECK(v.back() == 4);

    v.reserve(64);
    CHECK(v.capacity() >= 64);
    CHECK(v.size() == 5);

    v.resize(2);
    CHECK(v.size() == 2);
    CHECK(v[1] == 1);

    v.resize(4);
    CHECK(v.size() == 4);
    CHECK(v[2] == 0); // value-initialized
    CHECK(v[3] == 0);

    v.clear();
    CHECK(v.empty());
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
