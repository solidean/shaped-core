#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// value/key type with deleted copy AND move — exercises the immovable path (nodes never move)
struct noncopy
{
    int v = 0;
    noncopy() = default;
    explicit noncopy(int x) : v(x) {}
    noncopy(noncopy const&) = delete;
    noncopy(noncopy&&) = delete;
    noncopy& operator=(noncopy const&) = delete;
    noncopy& operator=(noncopy&&) = delete;
};

// immovable key that hashes equal to its int and compares heterogeneously against int
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

// forces every key into one bucket, to stress collision chains
struct const_hash
{
    template <class T>
    u64 operator()(T const&) const
    {
        return 0;
    }
};
} // namespace

TEST("map - basic get / contains / operator[]")
{
    cc::map<int, int> m;
    CHECK(m.empty());
    CHECK(m.size() == 0);
    CHECK(m.get_ptr(1) == nullptr);
    CHECK(!m.contains(1));

    m[1] = 10;
    m[2] = 20;
    CHECK(m.size() == 2);
    CHECK(!m.empty());
    CHECK(m.contains(1));
    CHECK(*m.get_ptr(1) == 10);
    CHECK(*m.get_ptr(2) == 20);
    CHECK(m.get_ptr(3) == nullptr);

    m[1] = 11; // overwrite, no new entry
    CHECK(m.size() == 2);
    CHECK(*m.get_ptr(1) == 11);
}

TEST("map - operator[] default-inserts")
{
    cc::map<int, int> m;
    CHECK(m[5] == 0); // value-initialized
    CHECK(m.size() == 1);
    m[5] += 3;
    CHECK(*m.get_ptr(5) == 3);
}

TEST("map - get variants")
{
    cc::map<int, int> m;
    m[1] = 10;
    m[2] = 20;
    cc::map<int, int> const& cm = m;

    // get -> reference (asserts on absent); mutable overload allows write-through
    CHECK(m.get(1) == 10);
    CHECK(cm.get(2) == 20);
    m.get(1) = 11;
    CHECK(cm.get(1) == 11);

    // get_ptr -> pointer, nullptr if absent
    CHECK(*cm.get_ptr(2) == 20);
    CHECK(cm.get_ptr(3) == nullptr);

    // get_to -> bool, copies into out only on hit
    int out = -1;
    CHECK(cm.get_to(2, out));
    CHECK(out == 20);
    CHECK(!cm.get_to(3, out));
    CHECK(out == 20); // untouched on miss

    // get_or -> value or fallback (fallback may be a temporary)
    CHECK(cm.get_or(2, 99) == 20);
    CHECK(cm.get_or(3, 99) == 99);

    // get_or_default -> value or V{}
    CHECK(cm.get_or_default(2) == 20);
    CHECK(cm.get_or_default(3) == 0);
}

TEST("map - entry vacant then emplace")
{
    cc::map<int, cc::string> m;

    auto e = m.entry(42);
    CHECK(!e.exists());
    cc::string& v = e.emplace("hello");
    CHECK(e.exists());
    CHECK(v == "hello");
    CHECK(m.size() == 1);
    CHECK(*m.get_ptr(42) == "hello");
}

TEST("map - entry occupied and get_or_emplace")
{
    cc::map<int, int> m;
    m[7] = 70;

    auto e = m.entry(7);
    CHECK(e.exists());
    CHECK(e.key() == 7);
    CHECK(e.value() == 70);

    int factory_calls = 0;
    auto make = [&]
    {
        ++factory_calls;
        return 99;
    };
    CHECK(m.entry(7).get_or_emplace(make()) == 70); // hit: existing value
    CHECK(m.entry(8).get_or_emplace(80) == 80);     // miss: inserts
    CHECK(factory_calls == 1);                      // make() itself still evaluated once (eager arg)
    CHECK(m.size() == 2);
}

TEST("map - heterogeneous string_view lookup")
{
    cc::map<cc::string, int> m;
    m[cc::string("alpha")] = 1;
    m[cc::string("beta")] = 2;

    cc::string_view const sv = "alpha";
    CHECK(m.contains(sv));
    CHECK(*m.get_ptr(sv) == 1);
    CHECK(m.get_ptr(cc::string_view("gamma")) == nullptr);

    auto e = m.entry(cc::string_view("beta"));
    CHECK(e.exists());
    CHECK(e.value() == 2);
}

TEST("map - reference stability across growth")
{
    cc::map<int, int> m;
    m[0] = 1000;
    int* const pinned = m.get_ptr(0);
    int const* const kpinned = &m.entry(0).key();

    // force several rehashes
    for (int i = 1; i < 500; ++i)
        m[i] = i;

    CHECK(m.size() == 500);
    CHECK(m.get_ptr(0) == pinned); // node never moved
    CHECK(*pinned == 1000);        // value untouched
    CHECK(&m.entry(0).key() == kpinned);
    CHECK(m.bucket_count() >= 500); // grew

    // every entry still resolves
    for (int i = 0; i < 500; ++i)
        CHECK(*m.get_ptr(i) == (i == 0 ? 1000 : i));
}

TEST("map - immovable key and value")
{
    cc::map<imkey, noncopy> m;

    m.entry(5).emplace(99);
    m.entry(6).emplace(66);
    CHECK(m.size() == 2);

    noncopy* p = m.get_ptr(5); // heterogeneous probe with int
    REQUIRE(p != nullptr);
    CHECK(p->v == 99);
    CHECK(m.get_ptr(6)->v == 66);
    CHECK(m.get_ptr(7) == nullptr);

    // moving the map keeps the immovable nodes in place
    noncopy* const before = m.get_ptr(5);
    cc::map<imkey, noncopy> m2 = cc::move(m);
    CHECK(m2.get_ptr(5) == before);
    CHECK(m2.get_ptr(5)->v == 99);
    CHECK(m.size() == 0);
}

TEST("map - erase")
{
    cc::map<int, int> m;
    for (int i = 0; i < 10; ++i)
        m[i] = i * i;

    CHECK(!m.erase(100)); // absent
    CHECK(m.erase(3));
    CHECK(m.size() == 9);
    CHECK(m.get_ptr(3) == nullptr);
    CHECK(*m.get_ptr(4) == 16); // neighbors intact

    // erase everything
    for (int i = 0; i < 10; ++i)
        m.erase(i);
    CHECK(m.empty());
}

TEST("map - clear and reserve")
{
    cc::map<int, int> m;
    m.reserve(200);
    CHECK(m.bucket_count() >= 200);
    isize const cap_after_reserve = m.bucket_count();

    for (int i = 0; i < 100; ++i)
        m[i] = i;
    CHECK(m.bucket_count() == cap_after_reserve); // no rehash needed

    m.clear();
    CHECK(m.empty());
    CHECK(m.get_ptr(0) == nullptr);
    CHECK(m.bucket_count() == cap_after_reserve); // buckets retained
    m[1] = 1;                                     // usable after clear
    CHECK(*m.get_ptr(1) == 1);
}

TEST("map - collision chains (adversarial hash)")
{
    cc::map<int, int, const_hash> m;
    for (int i = 0; i < 50; ++i)
        m[i] = i + 1;

    CHECK(m.size() == 50);
    for (int i = 0; i < 50; ++i)
        CHECK(*m.get_ptr(i) == i + 1);

    CHECK(m.erase(25)); // mid-chain erase
    CHECK(m.get_ptr(25) == nullptr);
    CHECK(*m.get_ptr(24) == 25);
    CHECK(*m.get_ptr(26) == 27);

    int sum = 0;
    for (auto [k, v] : m)
        sum += v;
    CHECK(sum == (1 + 50) * 50 / 2 - 26); // all values minus the erased one (26)
}

TEST("map - iteration visits each entry once and proxy is mutable")
{
    cc::map<int, int> m;
    for (int i = 0; i < 20; ++i)
        m[i] = i;

    int count = 0;
    int key_sum = 0;
    for (auto [k, v] : m)
    {
        ++count;
        key_sum += k;
        v += 100; // mutate through the proxy
    }
    CHECK(count == 20);
    CHECK(key_sum == 19 * 20 / 2);

    for (int i = 0; i < 20; ++i)
        CHECK(*m.get_ptr(i) == i + 100);
}

TEST("map - iteration over empty map")
{
    cc::map<int, int> m;
    int count = 0;
    for (auto [k, v] : m)
    {
        (void)k;
        (void)v;
        ++count;
    }
    CHECK(count == 0);
}

TEST("map - conditional deep copy")
{
    cc::map<int, int> a;
    for (int i = 0; i < 30; ++i)
        a[i] = i * 2;

    cc::map<int, int> b = a; // copy
    a[0] = 999;
    a[100] = 100;

    CHECK(b.size() == 30);
    CHECK(*b.get_ptr(0) == 0); // independent of a's mutation
    CHECK(b.get_ptr(100) == nullptr);
    CHECK(*a.get_ptr(0) == 999);

    cc::map<int, int> c;
    c = b; // copy-assign
    CHECK(c.size() == 30);
    CHECK(*c.get_ptr(15) == 30);
}

// copy must be disabled when the value (or key) is non-copyable
static_assert(std::is_copy_constructible_v<cc::map<int, int>>);
static_assert(!std::is_copy_constructible_v<cc::map<int, noncopy>>);
static_assert(!std::is_copy_constructible_v<cc::map<imkey, int>>);
// but move is always available (nodes never move, only the bucket array pointer)
static_assert(std::is_move_constructible_v<cc::map<int, noncopy>>);

TEST("map - move construction and assignment")
{
    cc::map<int, int> a;
    for (int i = 0; i < 40; ++i)
        a[i] = i;

    cc::map<int, int> b = cc::move(a);
    CHECK(b.size() == 40);
    CHECK(*b.get_ptr(20) == 20);
    CHECK(a.empty()); // moved-from is empty but valid

    cc::map<int, int> c;
    c[999] = 1;
    c = cc::move(b);
    CHECK(c.size() == 40);
    CHECK(*c.get_ptr(39) == 39);
    CHECK(c.get_ptr(999) == nullptr); // old contents released
    CHECK(b.empty());
}
