#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// counts live instances so we can assert that destruction happens exactly once
struct Tracked
{
    static inline int ctor_count = 0;
    static inline int dtor_count = 0;
    static void reset_counters()
    {
        ctor_count = 0;
        dtor_count = 0;
    }

    int value = 0;

    explicit Tracked(int v = 0) : value(v) { ++ctor_count; }
    ~Tracked() { ++dtor_count; }

    Tracked(Tracked const&) = delete;
    Tracked& operator=(Tracked const&) = delete;
};

struct Point
{
    int x = 0;
    int y = 0;
};
} // namespace

TEST("unique_ptr - default construction is empty")
{
    cc::unique_ptr<int> p;
    CHECK(!p.is_valid());
    CHECK(!bool(p));
    CHECK(p.get() == nullptr);
    CHECK(p == nullptr);

    cc::unique_ptr<int> q = nullptr;
    CHECK(!q.is_valid());
}

TEST("unique_ptr - make_unique and access")
{
    SECTION("scalar value")
    {
        auto p = cc::make_unique<int>(42);
        REQUIRE(p.is_valid());
        CHECK(bool(p));
        CHECK(p.get() != nullptr);
        CHECK(*p == 42);

        *p = 7;
        CHECK(*p == 7);
    }

    SECTION("struct via operator->")
    {
        auto p = cc::make_unique<Point>(3, 4);
        REQUIRE(p.is_valid());
        CHECK(p->x == 3);
        CHECK(p->y == 4);
        p->x = 10;
        CHECK((*p).x == 10);
    }
}

TEST("unique_ptr - move transfers ownership")
{
    auto p = cc::make_unique<int>(123);
    int* raw = p.get();
    REQUIRE(raw != nullptr);

    SECTION("move construction empties source")
    {
        cc::unique_ptr<int> q = cc::move(p);
        CHECK(!p.is_valid());
        CHECK(q.is_valid());
        CHECK(q.get() == raw);
        CHECK(*q == 123);
    }

    SECTION("move assignment empties source")
    {
        cc::unique_ptr<int> q;
        q = cc::move(p);
        CHECK(!p.is_valid());
        CHECK(q.get() == raw);
    }
}

TEST("unique_ptr - destruction runs once")
{
    Tracked::reset_counters();

    SECTION("scope exit")
    {
        {
            auto p = cc::make_unique<Tracked>(1);
            CHECK(Tracked::ctor_count == 1);
            CHECK(Tracked::dtor_count == 0);
        }
        CHECK(Tracked::dtor_count == 1);
    }

    SECTION("assigning nullptr clears and destroys")
    {
        auto p = cc::make_unique<Tracked>(1);
        CHECK(Tracked::dtor_count == 0);
        p = nullptr;
        CHECK(Tracked::dtor_count == 1);
        CHECK(!p.is_valid());
        CHECK(p == nullptr);

        // assigning nullptr again is a no-op
        p = nullptr;
        CHECK(Tracked::dtor_count == 1);
    }

    SECTION("move-assign destroys the overwritten target")
    {
        auto a = cc::make_unique<Tracked>(1);
        auto b = cc::make_unique<Tracked>(2);
        CHECK(Tracked::ctor_count == 2);
        a = cc::move(b);
        CHECK(Tracked::dtor_count == 1); // a's original Tracked destroyed
        CHECK(a->value == 2);
        CHECK(!b.is_valid());
    }
}

TEST("unique_ptr - comparison")
{
    auto p = cc::make_unique<int>(1);
    auto q = cc::make_unique<int>(1);

    CHECK(p == p);
    CHECK(p != q); // distinct allocations
    CHECK(p != nullptr);
    CHECK(nullptr != p);

    CHECK(p == p.get());
    CHECK(p.get() == p);
    CHECK(p != q.get());

    cc::unique_ptr<int> empty;
    CHECK(empty == nullptr);
    CHECK(nullptr == empty);
}

TEST("unique_ptr - hash matches pointer identity")
{
    auto p = cc::make_unique<int>(5);
    CHECK(hash(p) == reinterpret_cast<u64>(p.get()));

    cc::unique_ptr<int> empty;
    CHECK(hash(empty) == 0);
}
