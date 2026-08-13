#include <clean-core/container/pair.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_debug_string.hh>
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

    static void reset_counters()
    {
        default_ctor_count = 0;
        copy_ctor_count = 0;
        move_ctor_count = 0;
        dtor_count = 0;
    }
};
} // namespace

// tuple propagates triviality from its elements
static_assert(std::is_trivially_copyable_v<cc::tuple<int, float>>);
static_assert(std::is_trivially_destructible_v<cc::tuple<int, float>>);
static_assert(std::is_trivially_copyable_v<cc::tuple<int, cc::pair<int, float>>>);
static_assert(std::is_trivially_copyable_v<cc::tuple<>>);
static_assert(!std::is_trivially_copyable_v<cc::tuple<cc::string>>);
static_assert(!std::is_trivially_destructible_v<cc::tuple<cc::string>>);

// move-only elements make the tuple move-only, without deleting the move
static_assert(!std::is_copy_constructible_v<cc::tuple<int, move_only>>);
static_assert(!std::is_copy_assignable_v<cc::tuple<int, move_only>>);
static_assert(std::is_move_constructible_v<cc::tuple<int, move_only>>);
static_assert(std::is_move_assignable_v<cc::tuple<int, move_only>>);

// layout is the plain concatenation of the elements
static_assert(sizeof(cc::tuple<int, float>) == 8);
static_assert(sizeof(cc::tuple<int, int, int>) == 12);

// the tuple protocol, which is what structured bindings run on
static_assert(std::tuple_size_v<cc::tuple<int, float>> == 2);
static_assert(std::tuple_size_v<cc::tuple<>> == 0);
static_assert(std::is_same_v<std::tuple_element_t<0, cc::tuple<int, float>>, int>);
static_assert(std::is_same_v<std::tuple_element_t<1, cc::tuple<int, float>>, float>);
static_assert(cc::tuple<int, float>::element_count == 2);
static_assert(std::is_same_v<cc::tuple<int, float>::element_t<1>, float>);

// duplicate element types are fine, since access is index-based only
static_assert(std::tuple_size_v<cc::tuple<int, int>> == 2);
static_assert(std::is_same_v<std::tuple_element_t<1, cc::tuple<int, int>>, int>);

// CTAD decays
static_assert(std::is_same_v<decltype(cc::tuple{1, 2.5f}), cc::tuple<int, float>>);

// the member get<I> forwards the tuple's value category too
static_assert(std::is_same_v<decltype(std::declval<cc::tuple<int, float>&>().get<0>()), int&>);
static_assert(std::is_same_v<decltype(std::declval<cc::tuple<int, float> const&>().get<0>()), int const&>);
static_assert(std::is_same_v<decltype(std::declval<cc::tuple<int, float>&&>().get<0>()), int&&>);

// get<I> forwards the tuple's value category
static_assert(std::is_same_v<decltype(get<0>(std::declval<cc::tuple<int, float>&>())), int&>);
static_assert(std::is_same_v<decltype(get<0>(std::declval<cc::tuple<int, float> const&>())), int const&>);
static_assert(std::is_same_v<decltype(get<0>(std::declval<cc::tuple<int, float>&&>())), int&&>);
static_assert(std::is_same_v<decltype(get<0>(std::declval<cc::tuple<int, float> const&&>())), int const&&>);

// usable in a constant expression
static_assert(get<1>(cc::tuple{1, 2.5f}) == 2.5f);
static_assert(cc::tuple{1, 2} == cc::tuple{1, 2});
static_assert(cc::tuple{1, 2} < cc::tuple{1, 3});

TEST("tuple - construction and access")
{
    SECTION("element-wise")
    {
        auto t = cc::tuple<int, float, cc::string>(1, 2.5f, "hi");
        CHECK(get<0>(t) == 1);
        CHECK(get<1>(t) == 2.5f);
        CHECK(get<2>(t) == "hi");
    }

    SECTION("default construction value-initializes")
    {
        auto t = cc::tuple<int, float>();
        CHECK(get<0>(t) == 0);
        CHECK(get<1>(t) == 0.0f);
    }

    SECTION("narrowing element conversions are allowed")
    {
        auto t = cc::tuple<float, int>(1, 2.5f);
        CHECK(get<0>(t) == 1.0f);
        CHECK(get<1>(t) == 2);
    }

    SECTION("mutation through get")
    {
        auto t = cc::tuple{1, 2.5f};
        get<0>(t) = 7;
        CHECK(get<0>(t) == 7);
    }

    SECTION("the member get agrees with the free one")
    {
        auto t = cc::tuple{1, 2.5f};
        CHECK(t.get<0>() == 1);
        t.get<0>() = 7;
        CHECK(get<0>(t) == 7);

        auto const& c = t;
        CHECK(c.get<1>() == 2.5f);
    }

    SECTION("empty tuple")
    {
        auto t = cc::tuple<>();
        auto const u = t;
        CHECK(t == u);
        CHECK(cc::make_hash(t) == cc::make_hash(u));
    }

    SECTION("nesting")
    {
        auto t = cc::tuple{cc::tuple{1, 2}, 3};
        CHECK(get<1>(get<0>(t)) == 2);
        CHECK(get<1>(t) == 3);
    }
}

TEST("tuple - structured bindings")
{
    SECTION("by value")
    {
        auto const t = cc::tuple{1, 2.5f};
        auto const [a, b] = t;
        CHECK(a == 1);
        CHECK(b == 2.5f);
    }

    SECTION("by reference, mutating")
    {
        auto t = cc::tuple{1, 2.5f};
        auto& [a, b] = t;
        a = 7;
        CHECK(get<0>(t) == 7);
    }
}

TEST("tuple - move-only elements")
{
    auto t = cc::tuple<int, move_only>(1, move_only(5));
    CHECK(get<1>(t).value == 5);

    auto u = cc::move(t);
    CHECK(get<1>(u).value == 5);
    CHECK(get<1>(t).value == -1);
    CHECK(get<0>(u) == 1);

    auto v = cc::tuple<int, move_only>();
    v = cc::move(u);
    CHECK(get<1>(v).value == 5);
}

TEST("tuple - element lifetime")
{
    counting_type::reset_counters();

    {
        auto t = cc::tuple<counting_type, int>(counting_type(3), 1);
        CHECK(counting_type::default_ctor_count == 1);
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(get<0>(t).value == 3);
    }

    // the argument temporary plus the element inside the tuple
    CHECK(counting_type::dtor_count == 2);

    counting_type::reset_counters();
    {
        auto const t = cc::tuple<counting_type>(counting_type(1));
        auto u = t;
        get<0>(u).value = 2;
        CHECK(counting_type::copy_ctor_count == 1);
        CHECK(get<0>(t).value == 1);
        CHECK(get<0>(u).value == 2);
    }
    CHECK(counting_type::dtor_count == 3);
}

TEST("tuple - emplace")
{
    SECTION("replaces the element in place")
    {
        auto t = cc::tuple<int, cc::string>(1, "hi");
        auto& s = t.emplace<1>("hello");
        CHECK(s == "hello");
        CHECK(get<1>(t) == "hello");
        CHECK(get<0>(t) == 1);
    }

    SECTION("needs no assignment operator")
    {
        auto t = cc::tuple<int, move_only>();
        t.emplace<1>(5);
        CHECK(get<1>(t).value == 5);
    }

    SECTION("destroys the previous element exactly once")
    {
        counting_type::reset_counters();

        {
            auto t = cc::tuple<counting_type>();
            t.emplace<0>(4);
            CHECK(counting_type::dtor_count == 1);
            CHECK(get<0>(t).value == 4);
        }

        CHECK(counting_type::dtor_count == 2);
    }
}

TEST("tuple - comparison and hashing")
{
    SECTION("equality and order")
    {
        CHECK((cc::tuple{1, 2.0f} == cc::tuple{1, 2.0f}));
        CHECK((cc::tuple{1, 2.0f} != cc::tuple{1, 3.0f}));
        CHECK((cc::tuple{1, 2.0f} < cc::tuple{1, 3.0f}));
        CHECK((cc::tuple{1, 9.0f} < cc::tuple{2, 0.0f}));
        CHECK((cc::tuple{2, 0.0f} > cc::tuple{1, 9.0f}));
    }

    SECTION("hash is structural and order-dependent")
    {
        CHECK(cc::make_hash(cc::tuple{1, 2}) == cc::make_hash(cc::tuple{1, 2}));
        CHECK(cc::make_hash(cc::tuple{1, 2}) != cc::make_hash(cc::tuple{2, 1}));
    }
}

TEST("tuple - apply")
{
    SECTION("value-returning")
    {
        auto const r = cc::apply([](int a, float b) { return a + b; }, cc::tuple{1, 2.5f});
        CHECK(r == 3.5f);
    }

    SECTION("void-returning")
    {
        auto sum = 0;
        cc::apply([&](int a, int b) { sum = a + b; }, cc::tuple{1, 2});
        CHECK(sum == 3);
    }

    SECTION("forwards the value category")
    {
        auto t = cc::tuple<move_only>(move_only(5));
        auto const v = cc::apply([](move_only&& m) { return move_only(cc::move(m)); }, cc::move(t));
        CHECK(v.value == 5);
    }

    SECTION("works on other tuple-likes")
    {
        auto const r = cc::apply([](int a, int b) { return a * b; }, cc::pair<int, int>{3, 4});
        CHECK(r == 12);
    }
}

TEST("tuple - debug string")
{
    CHECK(cc::to_debug_string(cc::tuple{1, 2}) == "(1, 2)");
    CHECK(cc::to_debug_string(cc::pair<int, int>{1, 2}) == "(1, 2)");
}
