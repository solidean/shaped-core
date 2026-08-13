#include <clean-core/container/variant.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

#include <type_traits>

using namespace cc::primitive_defines;

namespace
{
// move-only type for testing
struct move_only
{
    int value = 0;
    move_only() = default;
    explicit move_only(int v) : value(v) {}
    move_only(move_only const&) = delete;
    move_only(move_only&& rhs) noexcept : value(rhs.value) { rhs.value = -1; }
    move_only& operator=(move_only const&) = delete;
    move_only& operator=(move_only&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }
    ~move_only() = default;
};

// neither movable nor copyable, so it can only be built in place
struct immovable
{
    int value = 0;
    explicit immovable(int v) : value(v) {}
    immovable(immovable const&) = delete;
    immovable(immovable&&) = delete;
    immovable& operator=(immovable const&) = delete;
    immovable& operator=(immovable&&) = delete;
    ~immovable() = default;
};

// counts its own lifetime events
struct counting_type
{
    inline static int default_ctor_count = 0;
    inline static int copy_ctor_count = 0;
    inline static int move_ctor_count = 0;
    inline static int dtor_count = 0;

    int value = 0;

    counting_type() { ++default_ctor_count; }
    explicit counting_type(int v) : value(v) { ++default_ctor_count; }
    counting_type(counting_type const& rhs) : value(rhs.value) { ++copy_ctor_count; }
    counting_type(counting_type&& rhs) noexcept : value(rhs.value) { ++move_ctor_count; }
    counting_type& operator=(counting_type const& rhs) = default;
    counting_type& operator=(counting_type&& rhs) noexcept
    {
        value = rhs.value;
        return *this;
    }
    ~counting_type() { ++dtor_count; }

    [[nodiscard]] friend bool operator==(counting_type const& a, counting_type const& b) { return a.value == b.value; }

    static void reset_counters()
    {
        default_ctor_count = 0;
        copy_ctor_count = 0;
        move_ctor_count = 0;
        dtor_count = 0;
    }
};
} // namespace

// variant propagates triviality from its alternatives
static_assert(std::is_trivially_copyable_v<cc::variant<int, float>>);
static_assert(std::is_trivially_destructible_v<cc::variant<int, float>>);
static_assert(!std::is_trivially_copyable_v<cc::variant<int, cc::string>>);
static_assert(!std::is_trivially_destructible_v<cc::variant<int, cc::string>>);

// move-only alternatives make the variant move-only
static_assert(!std::is_copy_constructible_v<cc::variant<int, move_only>>);
static_assert(!std::is_copy_assignable_v<cc::variant<int, move_only>>);
static_assert(std::is_move_constructible_v<cc::variant<int, move_only>>);
static_assert(std::is_move_assignable_v<cc::variant<int, move_only>>);

// an immovable alternative makes the whole variant immovable
static_assert(!std::is_move_constructible_v<cc::variant<int, immovable>>);
static_assert(!std::is_copy_constructible_v<cc::variant<int, immovable>>);

// the index tag fills the storage's tail padding instead of adding bytes
static_assert(sizeof(cc::variant<u8, u8>) == 2);
static_assert(sizeof(cc::variant<u32, u8>) == 8);
static_assert(sizeof(cc::variant<u32, u32>) == 8);

static_assert(cc::variant<int, float>::alternative_count == 2);
static_assert(std::is_same_v<cc::variant<int, float>::alternative_t<1>, float>);

// the value constructor takes exact type matches only
static_assert(std::is_constructible_v<cc::variant<int, cc::string>, int>);
static_assert(!std::is_constructible_v<cc::variant<int, cc::string>, char const*>);
static_assert(!std::is_constructible_v<cc::variant<int, cc::string>, float>);
// ... and stays out of the way when the alternative is duplicated
static_assert(!std::is_constructible_v<cc::variant<int, int>, int>);

TEST("variant - construction")
{
    SECTION("default construction takes alternative 0")
    {
        auto const v = cc::variant<int, float>();
        CHECK(v.index() == 0);
        CHECK(v.visit([](int i) { return i; }, [](float) { return -1; }) == 0);
    }

    SECTION("exact-match value construction")
    {
        auto const v = cc::variant<int, cc::string>(cc::string("hi"));
        CHECK(v.index() == 1);

        auto const w = cc::variant<int, cc::string>(42);
        CHECK(w.index() == 0);
    }

    SECTION("create_emplaced picks the index explicitly")
    {
        auto const v = cc::variant<int, cc::string>::create_emplaced<1>("hello");
        CHECK(v.index() == 1);
        CHECK(v.visit([](int) { return cc::string(); }, [](cc::string const& s) { return s; }) == "hello");
    }

    SECTION("create_emplaced works for immovable alternatives")
    {
        auto const v = cc::variant<int, immovable>::create_emplaced<1>(7);
        CHECK(v.index() == 1);
        CHECK(v.visit([](int i) { return i; }, [](immovable const& m) { return m.value; }) == 7);
    }

    SECTION("duplicate alternatives stay distinct")
    {
        auto const a = cc::variant<int, int>::create_emplaced<0>(5);
        auto const b = cc::variant<int, int>::create_emplaced<1>(5);
        CHECK(a.index() == 0);
        CHECK(b.index() == 1);
        CHECK(a != b);
        CHECK(cc::make_hash(a) != cc::make_hash(b));
    }
}

TEST("variant - visitation")
{
    SECTION("one handler per alternative")
    {
        auto const v = cc::variant<int, float, cc::string>(2.5f);
        auto const s = v.visit([](int) { return cc::string("int"); },     //
                               [](float) { return cc::string("float"); }, //
                               [](cc::string const&) { return cc::string("string"); });
        CHECK(s == "float");
    }

    SECTION("a single generic handler")
    {
        auto const v = cc::variant<int, float>(3);
        CHECK(v.visit([](auto x) { return int(x); }) == 3);
    }

    SECTION("returning void")
    {
        auto const v = cc::variant<int, float>(7);
        auto seen = 0;
        v.visit([&](int i) { seen = i; }, [&](float) { seen = -1; });
        CHECK(seen == 7);
    }

    SECTION("returning a reference, and mutating through it")
    {
        auto v = cc::variant<int, int>::create_emplaced<0>(1);
        auto& r = v.visit([](int& i) -> int& { return i; });
        r = 9;
        CHECK(v.visit([](int i) { return i; }) == 9);
    }

    SECTION("const handlers see const")
    {
        auto const v = cc::variant<int, float>(1);
        CHECK(v.visit([](int const&) { return true; }, [](float const&) { return false; }));
    }

    SECTION("an rvalue variant can be moved out of")
    {
        auto v = cc::variant<int, move_only>::create_emplaced<1>(5);
        auto const m
            = cc::move(v).visit([](int) { return move_only(); }, [](move_only&& x) { return move_only(cc::move(x)); });
        CHECK(m.value == 5);
    }
}

TEST("variant - emplace")
{
    counting_type::reset_counters();

    {
        auto v = cc::variant<counting_type, int>();
        CHECK(v.index() == 0);
        CHECK(counting_type::default_ctor_count == 1);

        v.emplace<1>(7);
        CHECK(v.index() == 1);
        CHECK(counting_type::dtor_count == 1); // the previous alternative was destroyed

        auto& c = v.emplace<0>(3);
        CHECK(c.value == 3);
        CHECK(v.index() == 0);
    }

    CHECK(counting_type::dtor_count == 2);
}

TEST("variant - copy and move")
{
    SECTION("move-only alternatives")
    {
        auto v = cc::variant<int, move_only>::create_emplaced<1>(5);

        auto w = cc::move(v);
        CHECK(w.index() == 1);
        CHECK(w.visit([](int) { return 0; }, [](move_only const& m) { return m.value; }) == 5);

        auto u = cc::variant<int, move_only>(0);
        u = cc::move(w);
        CHECK(u.index() == 1);
        CHECK(u.visit([](int) { return 0; }, [](move_only const& m) { return m.value; }) == 5);
    }

    SECTION("assignment across differing indices")
    {
        counting_type::reset_counters();

        auto a = cc::variant<counting_type, int>::create_emplaced<0>(1);
        auto const b = cc::variant<counting_type, int>::create_emplaced<1>(2);

        a = b;
        CHECK(a.index() == 1);
        CHECK(counting_type::dtor_count == 1); // a's counting_type went away

        a = cc::variant<counting_type, int>::create_emplaced<0>(3);
        CHECK(a.index() == 0);
        CHECK(a.visit([](counting_type const& c) { return c.value; }, [](int) { return -1; }) == 3);
    }

    SECTION("self-assignment is a no-op")
    {
        auto v = cc::variant<cc::string, int>(cc::string("hello"));
        auto& r = v;
        v = r;
        CHECK(v.index() == 0);
        CHECK(v.visit([](cc::string const& s) { return s; }, [](int) { return cc::string(); }) == "hello");
    }

    SECTION("trivial alternatives copy bitwise")
    {
        auto const v = cc::variant<int, float>(2.5f);
        auto const w = v;
        CHECK(w.index() == 1);
        CHECK(v == w);
    }
}

TEST("variant - comparison and hashing")
{
    SECTION("equality needs the same index")
    {
        CHECK((cc::variant<int, float>(1) == cc::variant<int, float>(1)));
        CHECK((cc::variant<int, float>(1) != cc::variant<int, float>(2)));
        CHECK((cc::variant<int, float>(1) != cc::variant<int, float>(1.0f)));
    }

    SECTION("hash combines the index with the active alternative")
    {
        auto const a = cc::variant<int, float>(1);
        auto const b = cc::variant<int, float>(1);
        CHECK(cc::make_hash(a) == cc::make_hash(b));
        CHECK(cc::make_hash(a) != cc::make_hash(cc::variant<int, float>(2)));
    }
}
