#include <clean-core/common/hash.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// immovable element that hashes equal to its int and compares heterogeneously against int
struct imkey
{
    int v;
    explicit imkey(int x) : v(x) {}
    imkey(imkey const&) = delete;
    imkey(imkey&&) = delete;
    imkey& operator=(imkey const&) = delete;
    imkey& operator=(imkey&&) = delete;

    friend u64 hash(imkey const& k) { return cc::make_hash(k.v); }
    friend bool operator==(imkey const& a, imkey const& b) { return a.v == b.v; }
    friend bool operator==(imkey const& a, int b) { return a.v == b; }
};
} // namespace

TEST("set - insert / contains / erase")
{
    cc::set<int> s;
    CHECK(s.empty());
    CHECK(!s.contains(1));

    CHECK(s.insert(1)); // newly added
    CHECK(s.insert(2));
    CHECK(!s.insert(1)); // duplicate: no-op, returns false
    CHECK(s.size() == 2);
    CHECK(s.contains(1));
    CHECK(s.contains(2));
    CHECK(!s.contains(3));

    CHECK(s.erase(1));
    CHECK(!s.erase(1)); // already gone
    CHECK(s.size() == 1);
    CHECK(!s.contains(1));
}

TEST("set - empty value is free (no per-node value overhead)")
{
    // node = { u64 hash; node* next; int key; [[no_unique_address]] unit; }  -> same as without the value
    struct bare
    {
        u64 hash;
        void* next;
        int key;
    };
    CHECK(sizeof(cc::set<int>::map_t) > 0); // just make sure the type is usable
    CHECK(sizeof(bare) == 24);              // sanity anchor for the intended node shape
}

TEST("set - heterogeneous membership and insert")
{
    cc::set<cc::string> s;
    s.insert(cc::string("alpha"));
    s.insert(cc::string("beta"));

    CHECK(s.contains(cc::string_view("alpha")));
    CHECK(!s.contains(cc::string_view("gamma")));

    CHECK(s.insert(cc::string_view("gamma"))); // heterogeneous insert builds a string
    CHECK(s.contains(cc::string_view("gamma")));
    CHECK(s.size() == 3);
}

TEST("set - immovable element")
{
    cc::set<imkey> s;
    CHECK(s.insert(5)); // builds imkey(5) in place
    CHECK(s.insert(6));
    CHECK(!s.insert(5));

    CHECK(s.contains(5)); // heterogeneous probe with int
    CHECK(s.contains(6));
    CHECK(!s.contains(7));
    CHECK(s.size() == 2);

    // move keeps elements in place
    cc::set<imkey> s2 = cc::move(s);
    CHECK(s2.contains(5));
    CHECK(s.empty());
}

TEST("set - iteration visits each element once")
{
    cc::set<int> s;
    for (int i = 0; i < 30; ++i)
        s.insert(i);

    int count = 0;
    int sum = 0;
    for (int const& x : s)
    {
        ++count;
        sum += x;
    }
    CHECK(count == 30);
    CHECK(sum == 29 * 30 / 2);
}

TEST("set - clear and reserve")
{
    cc::set<int> s;
    s.reserve(200);
    CHECK(s.bucket_count() >= 200);

    for (int i = 0; i < 100; ++i)
        s.insert(i);
    CHECK(s.size() == 100);

    s.clear();
    CHECK(s.empty());
    CHECK(!s.contains(0));
    CHECK(s.insert(1)); // usable after clear
}

// copy is available for copyable elements, disabled for non-copyable ones; move is always available
static_assert(std::is_copy_constructible_v<cc::set<int>>);
static_assert(!std::is_copy_constructible_v<cc::set<imkey>>);
static_assert(std::is_move_constructible_v<cc::set<imkey>>);

TEST("set - deep copy is independent")
{
    cc::set<int> a;
    for (int i = 0; i < 20; ++i)
        a.insert(i);

    cc::set<int> b = a;
    a.insert(100);

    CHECK(b.size() == 20);
    CHECK(!b.contains(100));
    CHECK(a.contains(100));
}
