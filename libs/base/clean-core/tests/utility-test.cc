#include <clean-core/assert-handler.hh>
#include <clean-core/utility.hh>

#include <nexus/test.hh>

#include <cstdint>


// =========================================================================================================
// Helper types for testing
// =========================================================================================================

// A simple box type with comparison for min/max/clamp testing
struct Box
{
    int v;
    bool operator<(Box const& rhs) const { return v < rhs.v; }
};

// Move-only type with tracking for move/exchange/swap tests
struct MoveOnly
{
    int id;
    inline static int move_ctor_count = 0;
    inline static int move_assign_count = 0;

    explicit MoveOnly(int i = 0) : id(i) {}
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : id(other.id)
    {
        other.id = -1;
        ++move_ctor_count;
    }
    MoveOnly& operator=(MoveOnly&& other) noexcept
    {
        id = other.id;
        other.id = -1;
        ++move_assign_count;
        return *this;
    }

    static void reset_counts()
    {
        move_ctor_count = 0;
        move_assign_count = 0;
    }
};

// Type with ADL swap for testing ADL-awareness
namespace test_ns
{
struct AdlSwappable
{
    int value;
    inline static int adl_swap_count = 0;

    static void reset_count() { adl_swap_count = 0; }
};

void swap(AdlSwappable& a, AdlSwappable& b) noexcept
{
    ++AdlSwappable::adl_swap_count;
    int const tmp = a.value;
    a.value = b.value;
    b.value = tmp;
}
} // namespace test_ns

// =========================================================================================================
// Move semantics tests
// =========================================================================================================

TEST("utility - move returns T&& and selects rvalue overload")
{
    // Compile-time check: move returns rvalue reference
    int x = 5;
    static_assert(std::is_same_v<decltype(cc::move(x)), int&&>);

    // Test that move selects rvalue overload
    MoveOnly::reset_counts();
    MoveOnly mo(42);
    MoveOnly mo2(cc::move(mo)); // should call move ctor
    CHECK(mo2.id == 42);
    CHECK(mo.id == -1);
    CHECK(MoveOnly::move_ctor_count == 1);
}

TEST("utility - forward preserves value categories")
{
    struct Overloaded
    {
        static int call_lvalue(int&) { return 1; }
        static int call_rvalue(int&&) { return 2; }
        static int call_const_lvalue(int const&) { return 3; }
    };

    auto wrapper = [](auto&& arg) -> int
    {
        using T = decltype(arg);
        if constexpr (std::is_lvalue_reference_v<T>)
        {
            if constexpr (std::is_const_v<std::remove_reference_t<T>>)
                return Overloaded::call_const_lvalue(cc::forward<T>(arg));
            else
                return Overloaded::call_lvalue(cc::forward<T>(arg));
        }
        else
        {
            return Overloaded::call_rvalue(cc::forward<T>(arg));
        }
    };

    int lval = 5;
    int const const_lval = 6;

    CHECK(wrapper(lval) == 1);       // lvalue -> lvalue overload
    CHECK(wrapper(10) == 2);         // rvalue -> rvalue overload
    CHECK(wrapper(const_lval) == 3); // const lvalue -> const overload

    // Compile-time check
    static_assert(std::is_same_v<decltype(cc::forward<int&>(lval)), int&>);
    static_assert(std::is_same_v<decltype(cc::forward<int&&>(10)), int&&>);
}

TEST("utility - exchange replaces value and returns old")
{
    SECTION("integer exchange")
    {
        int x = 5;
        int old = cc::exchange(x, 9);
        CHECK(old == 5);
        CHECK(x == 9);
    }

    SECTION("move-only exchange")
    {
        MoveOnly::reset_counts();
        MoveOnly a(10);
        MoveOnly b(20);
        MoveOnly old = cc::exchange(a, cc::move(b));
        CHECK(old.id == 10);
        CHECK(a.id == 20);
        CHECK(b.id == -1); // b was moved
    }

    SECTION("exchange with convertible type")
    {
        int x = 100;
        int old = cc::exchange(x, 200);
        CHECK(old == 100);
        CHECK(x == 200);
    }

    SECTION("self exchange")
    {
        int x = 42;
        int old = cc::exchange(x, x);
        CHECK(old == 42);
        CHECK(x == 42);
    }
}

// =========================================================================================================
// Comparison and clamping tests
// =========================================================================================================

TEST("utility - max/min return references and handle equality")
{
    SECTION("max with distinct values")
    {
        Box a{10}, b{20};
        Box const& result = cc::max(a, b);
        CHECK(&result == &b); // returns ref to larger
        CHECK(result.v == 20);
    }

    SECTION("max with equal values returns b")
    {
        Box a{15}, b{15};
        Box const& result = cc::max(a, b);
        CHECK(&result == &b); // when equal, max returns b
    }

    SECTION("min with distinct values")
    {
        Box a{10}, b{20};
        Box const& result = cc::min(a, b);
        CHECK(&result == &a); // returns ref to smaller
        CHECK(result.v == 10);
    }

    SECTION("min with equal values returns a")
    {
        Box a{15}, b{15};
        Box const& result = cc::min(a, b);
        CHECK(&result == &a); // when equal, min returns a
    }
}

TEST("utility - max/min initializer list returns value")
{
    SECTION("max with integers")
    {
        int result = cc::max({3, 7, 2, 9, 1});
        CHECK(result == 9);
    }

    SECTION("max with three values")
    {
        CHECK(cc::max({10, 20, 15}) == 20);
    }

    SECTION("max with single value")
    {
        CHECK(cc::max({42}) == 42);
    }

    SECTION("max with duplicates")
    {
        CHECK(cc::max({5, 9, 9, 3, 9, 1}) == 9);
    }

    SECTION("max with all equal")
    {
        CHECK(cc::max({7, 7, 7, 7}) == 7);
    }

    SECTION("min with integers")
    {
        int result = cc::min({3, 7, 2, 9, 1});
        CHECK(result == 1);
    }

    SECTION("min with three values")
    {
        CHECK(cc::min({10, 20, 15}) == 10);
    }

    SECTION("min with single value")
    {
        CHECK(cc::min({42}) == 42);
    }

    SECTION("min with duplicates")
    {
        CHECK(cc::min({5, 2, 9, 3, 2, 1}) == 1);
    }

    SECTION("min with all equal")
    {
        CHECK(cc::min({7, 7, 7, 7}) == 7);
    }

    SECTION("max with floats")
    {
        CHECK(cc::max({1.5f, 3.7f, 2.1f}) == 3.7f);
    }

    SECTION("min with floats")
    {
        CHECK(cc::min({1.5f, 3.7f, 2.1f}) == 1.5f);
    }

    SECTION("max with Box type")
    {
        Box result = cc::max({Box{10}, Box{30}, Box{20}});
        CHECK(result.v == 30);
    }

    SECTION("min with Box type")
    {
        Box result = cc::min({Box{10}, Box{30}, Box{20}});
        CHECK(result.v == 10);
    }

    SECTION("max with negative values")
    {
        CHECK(cc::max({-5, -1, -10, -3}) == -1);
    }

    SECTION("min with negative values")
    {
        CHECK(cc::min({-5, -1, -10, -3}) == -10);
    }

    SECTION("empty list triggers assertion")
    {
        CHECK_ASSERTS(cc::max(std::initializer_list<int>{}));
        CHECK_ASSERTS(cc::min(std::initializer_list<int>{}));
    }
}

TEST("utility - clamp returns correct reference")
{
    SECTION("value in middle")
    {
        Box v{15}, lo{10}, hi{20};
        Box const& result = cc::clamp(v, lo, hi);
        CHECK(&result == &v); // returns v when in range
        CHECK(result.v == 15);
    }

    SECTION("value below range")
    {
        Box v{5}, lo{10}, hi{20};
        Box const& result = cc::clamp(v, lo, hi);
        CHECK(&result == &lo); // returns lo when below
        CHECK(result.v == 10);
    }

    SECTION("value above range")
    {
        Box v{25}, lo{10}, hi{20};
        Box const& result = cc::clamp(v, lo, hi);
        CHECK(&result == &hi); // returns hi when above
        CHECK(result.v == 20);
    }

    SECTION("value equals lo")
    {
        Box v{10}, lo{10}, hi{20};
        Box const& result = cc::clamp(v, lo, hi);
        CHECK(&result == &v); // returns v when equal to boundary
    }

    SECTION("value equals hi")
    {
        Box v{20}, lo{10}, hi{20};
        Box const& result = cc::clamp(v, lo, hi);
        CHECK(&result == &v); // returns v when equal to boundary
    }

    SECTION("lo equals hi")
    {
        Box v{15}, boundary{10};
        Box const& result = cc::clamp(v, boundary, boundary);
        CHECK(&result == &boundary); // returns boundary when lo==hi
    }
}

// =========================================================================================================
// Wrapping arithmetic tests
// =========================================================================================================

TEST("utility - wrapped_increment wraps correctly")
{
    SECTION("max=1 wraps immediately")
    {
        CHECK(cc::wrapped_increment(0, 1) == 0);
    }

    SECTION("max=3 ring behavior")
    {
        CHECK(cc::wrapped_increment(0, 3) == 1);
        CHECK(cc::wrapped_increment(1, 3) == 2);
        CHECK(cc::wrapped_increment(2, 3) == 0); // wraps
    }

    SECTION("result always in range")
    {
        for (int i = 0; i < 10; ++i)
        {
            int result = cc::wrapped_increment(i, 10);
            CHECK(result >= 0);
            CHECK(result < 10);
        }
    }

    SECTION("unsigned types")
    {
        unsigned u = 5;
        CHECK(cc::wrapped_increment(u, 6u) == 0u);
        CHECK(cc::wrapped_increment(u, 7u) == 6u);
    }
}

TEST("utility - wrapped_decrement wraps correctly")
{
    SECTION("max=1 wraps immediately")
    {
        CHECK(cc::wrapped_decrement(0, 1) == 0);
    }

    SECTION("max=3 ring behavior")
    {
        CHECK(cc::wrapped_decrement(2, 3) == 1);
        CHECK(cc::wrapped_decrement(1, 3) == 0);
        CHECK(cc::wrapped_decrement(0, 3) == 2); // wraps
    }

    SECTION("result always in range")
    {
        for (int i = 0; i < 10; ++i)
        {
            int result = cc::wrapped_decrement(i, 10);
            CHECK(result >= 0);
            CHECK(result < 10);
        }
    }

    SECTION("unsigned types")
    {
        unsigned u = 0;
        CHECK(cc::wrapped_decrement(u, 6u) == 5u);
    }
}

// =========================================================================================================
// Integer division tests
// =========================================================================================================

TEST("utility - int_div_round_up rounds up correctly")
{
    SECTION("nom < denom")
    {
        CHECK(cc::int_div_round_up(1, 3) == 1);
        CHECK(cc::int_div_round_up(2, 3) == 1);
    }

    SECTION("exact multiples")
    {
        CHECK(cc::int_div_round_up(9, 3) == 3);
        CHECK(cc::int_div_round_up(12, 3) == 4);
    }

    SECTION("just over multiple")
    {
        CHECK(cc::int_div_round_up(10, 3) == 4);
        CHECK(cc::int_div_round_up(13, 3) == 5);
    }

    SECTION("denom = 1")
    {
        CHECK(cc::int_div_round_up(5, 1) == 5);
        CHECK(cc::int_div_round_up(100, 1) == 100);
    }

    SECTION("overflow avoidance with u64")
    {
        // max_u64 / 2 rounds up
        uint64_t const max_val = UINT64_MAX;
        uint64_t const result = cc::int_div_round_up(max_val, uint64_t(2));
        // UINT64_MAX / 2 = 9223372036854775807.5, rounds up to 9223372036854775808
        CHECK(result == (max_val / 2) + 1);
    }
}

TEST("utility - int_round_up_to_multiple rounds to next multiple")
{
    SECTION("val == 0")
    {
        CHECK(cc::int_round_up_to_multiple(0, 1) == 0);
        CHECK(cc::int_round_up_to_multiple(0, 2) == 0);
        CHECK(cc::int_round_up_to_multiple(0, 10) == 0);
    }

    SECTION("multiple == 1")
    {
        CHECK(cc::int_round_up_to_multiple(5, 1) == 5);
        CHECK(cc::int_round_up_to_multiple(100, 1) == 100);
    }

    SECTION("already a multiple")
    {
        CHECK(cc::int_round_up_to_multiple(30, 10) == 30);
        CHECK(cc::int_round_up_to_multiple(16, 8) == 16);
    }

    SECTION("round up")
    {
        CHECK(cc::int_round_up_to_multiple(23, 10) == 30);
        CHECK(cc::int_round_up_to_multiple(17, 8) == 24);
        CHECK(cc::int_round_up_to_multiple(1, 2) == 2);
    }

    SECTION("larger values")
    {
        CHECK(cc::int_round_up_to_multiple(1000, 256) == 1024);
        CHECK(cc::int_round_up_to_multiple(999, 256) == 1024);
    }
}

// =========================================================================================================
// Swap tests
// =========================================================================================================

TEST("utility - swap respects custom ADL swap")
{
    test_ns::AdlSwappable::reset_count();
    test_ns::AdlSwappable a{10}, b{20};

    cc::swap(a, b);

    CHECK(test_ns::AdlSwappable::adl_swap_count == 1); // ADL swap was called
    CHECK(a.value == 20);
    CHECK(b.value == 10);
}

TEST("utility - swap works with move-only types")
{
    MoveOnly::reset_counts();
    MoveOnly a(10), b(20);

    cc::swap(a, b);

    CHECK(a.id == 20);
    CHECK(b.id == 10);
    // At least some moves happened (not pinning exact count for flexibility)
    CHECK(MoveOnly::move_ctor_count + MoveOnly::move_assign_count > 0);
    // But no copies (move-only prevents this)
}

TEST("utility - swap_by_move bypasses custom swap")
{
    test_ns::AdlSwappable::reset_count();
    test_ns::AdlSwappable a{10}, b{20};

    cc::swap_by_move(a, b);

    CHECK(test_ns::AdlSwappable::adl_swap_count == 0); // ADL swap was NOT called
    CHECK(a.value == 20);
    CHECK(b.value == 10);
}

TEST("utility - swap_by_move works with move-only types")
{
    MoveOnly::reset_counts();
    MoveOnly a(30), b(40);

    cc::swap_by_move(a, b);

    CHECK(a.id == 40);
    CHECK(b.id == 30);
}

// =========================================================================================================
// Alignment tests
// =========================================================================================================

TEST("utility - is_power_of_two truth table")
{
    SECTION("powers of two")
    {
        CHECK(cc::is_power_of_two(1));
        CHECK(cc::is_power_of_two(2));
        CHECK(cc::is_power_of_two(4));
        CHECK(cc::is_power_of_two(8));
        CHECK(cc::is_power_of_two(16));
        CHECK(cc::is_power_of_two(1024));
    }

    SECTION("non-powers of two")
    {
        CHECK(!cc::is_power_of_two(3));
        CHECK(!cc::is_power_of_two(5));
        CHECK(!cc::is_power_of_two(6));
        CHECK(!cc::is_power_of_two(7));
        CHECK(!cc::is_power_of_two(9));
        CHECK(!cc::is_power_of_two(12));
        CHECK(!cc::is_power_of_two(100));
    }

    SECTION("signed types")
    {
        int s = 16;
        CHECK(cc::is_power_of_two(s));
        s = 17;
        CHECK(!cc::is_power_of_two(s));
    }
}

TEST("utility - align_up_masked numeric behavior")
{
    constexpr int mask15 = 15; // alignment 16

    CHECK(cc::align_up_masked(0, mask15) == 0);
    CHECK(cc::align_up_masked(1, mask15) == 16);
    CHECK(cc::align_up_masked(15, mask15) == 16);
    CHECK(cc::align_up_masked(16, mask15) == 16);
    CHECK(cc::align_up_masked(17, mask15) == 32);
    CHECK(cc::align_up_masked(300, mask15) == 304);

    SECTION("mask=0 (alignment 1)")
    {
        CHECK(cc::align_up_masked(42, 0) == 42);
        CHECK(cc::align_up_masked(99, 0) == 99);
    }
}

TEST("utility - align_down_masked numeric behavior")
{
    constexpr int mask15 = 15; // alignment 16

    CHECK(cc::align_down_masked(0, mask15) == 0);
    CHECK(cc::align_down_masked(1, mask15) == 0);
    CHECK(cc::align_down_masked(15, mask15) == 0);
    CHECK(cc::align_down_masked(16, mask15) == 16);
    CHECK(cc::align_down_masked(17, mask15) == 16);
    CHECK(cc::align_down_masked(300, mask15) == 288);

    SECTION("mask=0 (alignment 1)")
    {
        CHECK(cc::align_down_masked(42, 0) == 42);
        CHECK(cc::align_down_masked(99, 0) == 99);
    }
}

TEST("utility - align_up/down equivalence to masked versions")
{
    constexpr int test_values[] = {0, 1, 7, 8, 9, 15, 16, 17, 100, 255, 256, 300, 1000};
    constexpr int alignments[] = {1, 2, 8, 16, 256};

    for (int val : test_values)
    {
        for (int align : alignments)
        {
            CHECK(cc::align_up(val, align) == cc::align_up_masked(val, align - 1));
            CHECK(cc::align_down(val, align) == cc::align_down_masked(val, align - 1));
        }
    }

    SECTION("already aligned returns unchanged")
    {
        CHECK(cc::align_up(16, 16) == 16);
        CHECK(cc::align_down(16, 16) == 16);
        CHECK(cc::align_up(256, 256) == 256);
        CHECK(cc::align_down(256, 256) == 256);
    }

    SECTION("alignment 1 returns unchanged")
    {
        CHECK(cc::align_up(42, 1) == 42);
        CHECK(cc::align_down(42, 1) == 42);
        CHECK(cc::align_up(999, 1) == 999);
        CHECK(cc::align_down(999, 1) == 999);
    }
}

TEST("utility - is_aligned correctness")
{
    SECTION("alignment=1 always true")
    {
        CHECK(cc::is_aligned(0, 1));
        CHECK(cc::is_aligned(1, 1));
        CHECK(cc::is_aligned(42, 1));
        CHECK(cc::is_aligned(999, 1));
    }

    SECTION("alignment=16")
    {
        CHECK(cc::is_aligned(0, 16));
        CHECK(cc::is_aligned(16, 16));
        CHECK(cc::is_aligned(32, 16));
        CHECK(cc::is_aligned(48, 16));

        CHECK(!cc::is_aligned(1, 16));
        CHECK(!cc::is_aligned(15, 16));
        CHECK(!cc::is_aligned(17, 16));
        CHECK(!cc::is_aligned(31, 16));
    }

    SECTION("consistency with align_up/down")
    {
        constexpr int test_values[] = {0, 1, 5, 15, 16, 17, 100, 255, 256, 300};
        constexpr int alignments[] = {1, 2, 8, 16, 256};

        for (int val : test_values)
        {
            for (int align : alignments)
            {
                int aligned_up = cc::align_up(val, align);
                int aligned_down = cc::align_down(val, align);
                CHECK(cc::is_aligned(aligned_up, align));
                CHECK(cc::is_aligned(aligned_down, align));
            }
        }
    }
}

// =========================================================================================================
// Precondition failure (death) tests
// =========================================================================================================

TEST("utility - precondition failures trigger assertions")
{
    SECTION("wrapped_increment: max == 0")
    {
        CHECK_ASSERTS(cc::wrapped_increment(0, 0));
    }

    SECTION("wrapped_increment: max < 0")
    {
        CHECK_ASSERTS(cc::wrapped_increment(0, -1));
    }

    SECTION("wrapped_decrement: max == 0")
    {
        CHECK_ASSERTS(cc::wrapped_decrement(0, 0));
    }

    SECTION("wrapped_decrement: max < 0")
    {
        CHECK_ASSERTS(cc::wrapped_decrement(0, -5));
    }

    SECTION("int_div_round_up: nom <= 0")
    {
        CHECK_ASSERTS(cc::int_div_round_up(0, 5));
        CHECK_ASSERTS(cc::int_div_round_up(-1, 5));
    }

    SECTION("int_div_round_up: denom <= 0")
    {
        CHECK_ASSERTS(cc::int_div_round_up(5, 0));
        CHECK_ASSERTS(cc::int_div_round_up(5, -1));
    }

    SECTION("int_round_up_to_multiple: multiple <= 0")
    {
        CHECK_ASSERTS(cc::int_round_up_to_multiple(10, 0));
        CHECK_ASSERTS(cc::int_round_up_to_multiple(10, -5));
    }

    SECTION("is_power_of_two: value <= 0")
    {
        CHECK_ASSERTS(cc::is_power_of_two(0));
        CHECK_ASSERTS(cc::is_power_of_two(-1));
        CHECK_ASSERTS(cc::is_power_of_two(-16));
    }

    SECTION("align_up: alignment <= 0")
    {
        CHECK_ASSERTS(cc::align_up(100, 0));
        CHECK_ASSERTS(cc::align_up(100, -16));
    }

    SECTION("align_up: alignment not power of two")
    {
        CHECK_ASSERTS(cc::align_up(100, 3));
        CHECK_ASSERTS(cc::align_up(100, 12));
    }

    SECTION("align_down: alignment <= 0")
    {
        CHECK_ASSERTS(cc::align_down(100, 0));
        CHECK_ASSERTS(cc::align_down(100, -8));
    }

    SECTION("align_down: alignment not power of two")
    {
        CHECK_ASSERTS(cc::align_down(100, 3));
        CHECK_ASSERTS(cc::align_down(100, 12));
    }

    SECTION("is_aligned: alignment <= 0")
    {
        CHECK_ASSERTS(cc::is_aligned(100, 0));
        CHECK_ASSERTS(cc::is_aligned(100, -4));
    }

    SECTION("is_aligned: alignment not power of two")
    {
        CHECK_ASSERTS(cc::is_aligned(100, 3));
        CHECK_ASSERTS(cc::is_aligned(100, 12));
    }

    SECTION("clamp: hi < lo")
    {
        CHECK_ASSERTS(cc::clamp(5, 10, 0));
    }
}

// =========================================================================================================
// Callable utilities tests
// =========================================================================================================

TEST("utility - overloaded combines multiple callables")
{
    SECTION("basic overload resolution")
    {
        auto f = cc::overloaded([](int x) { return x * 2; }, [](float x) { return x * 3.0f; },
                                [](char const* s) { return s[0]; });

        CHECK(f(10) == 20);
        CHECK(f(5.0f) == 15.0f);
        CHECK(f("hello") == 'h');
    }

    SECTION("overloaded with different return types")
    {
        auto f = cc::overloaded([](int) { return 42; }, [](float) { return 3.14f; });

        static_assert(std::is_same_v<decltype(f(1)), int>);
        static_assert(std::is_same_v<decltype(f(1.0f)), float>);

        SUCCEED(); // just static checks
    }

    SECTION("overloaded with references")
    {
        int counter = 0;
        auto f = cc::overloaded([&](int) { counter += 1; }, [&](float) { counter += 10; });

        f(5);
        CHECK(counter == 1);
        f(3.0f);
        CHECK(counter == 11);
    }
}

TEST("utility - void_function returns void")
{
    SECTION("no arguments")
    {
        cc::void_function{}();
        SUCCEED(); // just checking it compiles and runs
    }

    SECTION("multiple arguments")
    {
        cc::void_function{}(1, 2, 3, "test", 3.14f);
        SUCCEED(); // just checking it compiles and runs
    }

    SECTION("used as callback")
    {
        auto execute = [](auto fn) { fn(10, 20); };
        execute(cc::void_function{});
        SUCCEED(); // just checking it compiles and runs
    }
}

TEST("utility - identify_function preserves value category")
{
    SECTION("lvalue reference")
    {
        int x = 42;
        int& ref = cc::identify_function{}(x);
        CHECK(&ref == &x);
        CHECK(ref == 42);
    }

    SECTION("rvalue reference")
    {
        static_assert(std::is_same_v<decltype(cc::identify_function{}(10)), int&&>);

        SUCCEED(); // just static checks
    }

    SECTION("const lvalue reference")
    {
        int const x = 42;
        int const& ref = cc::identify_function{}(x);
        CHECK(&ref == &x);
    }

    SECTION("modifying through reference")
    {
        int x = 10;
        cc::identify_function{}(x) = 20;
        CHECK(x == 20);
    }
}

TEST("utility - constant_function returns constant value")
{
    SECTION("integer constant")
    {
        auto f = cc::constant_function<42>{};
        CHECK(f() == 42);
        CHECK(f(1, 2, 3) == 42);
        CHECK(f("ignored") == 42);
    }

    SECTION("float constant")
    {
        auto f = cc::constant_function<3.14f>{};
        CHECK(f() == 3.14f);
        CHECK(f(100) == 3.14f);
    }

    SECTION("bool constant")
    {
        auto f = cc::constant_function<true>{};
        CHECK(f() == true);
        CHECK(f(false) == true);
    }
}

TEST("utility - projection_function returns nth argument")
{
    SECTION("first argument")
    {
        auto f = cc::projection_function<0>{};
        CHECK(f(10, 20, 30) == 10);
        CHECK(f(100) == 100);
    }

    SECTION("second argument")
    {
        auto f = cc::projection_function<1>{};
        CHECK(f(10, 20, 30) == 20);
        CHECK(f('a', 'b') == 'b');
    }

    SECTION("third argument")
    {
        auto f = cc::projection_function<2>{};
        CHECK(f(10, 20, 30) == 30);
        CHECK(f(1.0f, 2.0f, 3.0f) == 3.0f);
    }

    SECTION("preserves reference")
    {
        int a = 1, b = 2, c = 3;
        int& ref = cc::projection_function<1>{}(a, b, c);
        CHECK(&ref == &b);
        ref = 200;
        CHECK(b == 200);
    }

    SECTION("works with different types")
    {
        auto result = cc::projection_function<1>{}(10, 3.14f, "hello");
        static_assert(std::is_same_v<decltype(result), float>);
        CHECK(result == 3.14f);
    }
}

// =========================================================================================================
// Template metaprogramming utilities tests
// =========================================================================================================

TEST("utility - dont_deduce disables deduction")
{
    // This test verifies the type alias works correctly
    // Actual deduction behavior would be tested at compile time
    static_assert(std::is_same_v<cc::dont_deduce<int>, int>);
    static_assert(std::is_same_v<cc::dont_deduce<float>, float>);
    static_assert(std::is_same_v<cc::dont_deduce<char const*>, char const*>);
    SUCCEED(); // compile-time test only
}

TEST("utility - function_ptr converts signatures to pointers")
{
    SECTION("simple function")
    {
        using ptr_t = cc::function_ptr<int(float, double)>;
        static_assert(std::is_same_v<ptr_t, int (*)(float, double)>);

        SUCCEED(); // just static checks
    }

    SECTION("void return")
    {
        using ptr_t = cc::function_ptr<void()>;
        static_assert(std::is_same_v<ptr_t, void (*)()>);

        SUCCEED(); // just static checks
    }

    SECTION("noexcept function")
    {
        using ptr_t = cc::function_ptr<int(float) noexcept>;
        static_assert(std::is_same_v<ptr_t, int (*)(float) noexcept>);

        SUCCEED(); // just static checks
    }

    SECTION("function with many parameters")
    {
        using ptr_t = cc::function_ptr<char*(int, float, double, char)>;
        static_assert(std::is_same_v<ptr_t, char* (*)(int, float, double, char)>);

        SUCCEED(); // just static checks
    }

    SECTION("can be used with actual function pointers")
    {
        auto my_func = [](int x, int y) -> int { return x + y; };
        cc::function_ptr<int(int, int)> ptr = +my_func; // + converts lambda to function pointer
        CHECK(ptr(3, 4) == 7);
    }
}

// =========================================================================================================
// Scope utilities tests
// =========================================================================================================

TEST("utility - CC_DEFER executes at scope exit")
{
    SECTION("basic defer")
    {
        int counter = 0;
        {
            CC_DEFER
            {
                counter = 42;
            };
            CHECK(counter == 0); // not executed yet
        }
        CHECK(counter == 42); // executed at scope exit
    }

    SECTION("defer with multiple statements")
    {
        int a = 0, b = 0;
        {
            CC_DEFER
            {
                a = 10;
                b = 20;
            };
            CHECK(a == 0);
            CHECK(b == 0);
        }
        CHECK(a == 10);
        CHECK(b == 20);
    }

    SECTION("multiple defers execute in reverse order")
    {
        int order = 0;
        int first = 0, second = 0, third = 0;
        {
            CC_DEFER
            {
                first = ++order;
            };
            CC_DEFER
            {
                second = ++order;
            };
            CC_DEFER
            {
                third = ++order;
            };
        }
        CHECK(third == 1);  // executed first
        CHECK(second == 2); // executed second
        CHECK(first == 3);  // executed last
    }

    SECTION("defer with early return")
    {
        int cleanup = 0;
        auto test_func = [&]() -> int
        {
            CC_DEFER
            {
                cleanup = 1;
            };
            return 42;
        };
        int result = test_func();
        CHECK(result == 42);
        CHECK(cleanup == 1); // defer still executed
    }

    SECTION("defer captures by reference")
    {
        int value = 0;
        {
            CC_DEFER
            {
                value = 100;
            };
            value = 50; // modify before defer executes
        }
        CHECK(value == 100); // defer sees modified value
    }

    SECTION("defer can throw exceptions")
    {
        bool exception_caught = false;
        try
        {
            CC_DEFER
            {
                throw std::runtime_error("test exception");
            };
        }
        catch (std::runtime_error const& e)
        {
            exception_caught = true;
            CHECK(std::string(e.what()) == "test exception");
        }
        CHECK(exception_caught); // exception propagated from defer
    }
}

// =========================================================================================================
// Iterator utilities tests
// =========================================================================================================

TEST("utility - sentinel as end-of-range marker")
{
    SECTION("sentinel is default constructible")
    {
        cc::sentinel s;
        cc::sentinel s2{};

        SUCCEED(); // Just checking it compiles
    }

    SECTION("sentinel used with custom iterator")
    {
        struct counting_iterator
        {
            int count;
            int max;

            int operator*() const { return count; }
            counting_iterator& operator++()
            {
                ++count;
                return *this;
            }
            bool operator!=(cc::sentinel) const { return count < max; }
        };

        struct counting_range
        {
            int max;
            counting_iterator begin() const { return {0, max}; }
            cc::sentinel end() const { return {}; }
        };

        counting_range range{5};
        int sum = 0;
        for (int val : range)
        {
            sum += val;
        }
        CHECK(sum == 0 + 1 + 2 + 3 + 4);
    }

    SECTION("sentinel with pointer-based iterator")
    {
        struct array_view
        {
            int const* ptr;
            int size;

            struct iterator
            {
                int const* current;
                int const* end_ptr;

                int operator*() const { return *current; }
                iterator& operator++()
                {
                    ++current;
                    return *this;
                }
                bool operator!=(cc::sentinel) const { return current != end_ptr; }
            };

            iterator begin() const { return {ptr, ptr + size}; }
            cc::sentinel end() const { return {}; }
        };

        int arr[] = {10, 20, 30};
        array_view view{arr, 3};

        int sum = 0;
        for (int val : view)
        {
            sum += val;
        }
        CHECK(sum == 60);
    }
}

TEST("utility - begin/end with containers")
{
    SECTION("mutable container")
    {
        struct simple_container
        {
            int data[3] = {1, 2, 3};
            int* begin() { return data; }
            int* end() { return data + 3; }
        };

        simple_container c;
        CHECK(cc::begin(c) == c.data);
        CHECK(cc::end(c) == c.data + 3);

        // can modify through begin
        *cc::begin(c) = 10;
        CHECK(c.data[0] == 10);
    }

    SECTION("const container")
    {
        struct simple_container
        {
            int data[3] = {1, 2, 3};
            int const* begin() const { return data; }
            int const* end() const { return data + 3; }
        };

        simple_container const c{};
        CHECK(cc::begin(c) == c.data);
        CHECK(cc::end(c) == c.data + 3);
    }

    SECTION("container with both const and non-const")
    {
        struct dual_container
        {
            int data[3] = {1, 2, 3};
            int* begin() { return data; }
            int* end() { return data + 3; }
            int const* begin() const { return data; }
            int const* end() const { return data + 3; }
        };

        dual_container c;
        dual_container const& cc_ref = c;

        // non-const version
        static_assert(std::is_same_v<decltype(cc::begin(c)), int*>);
        static_assert(std::is_same_v<decltype(cc::end(c)), int*>);

        // const version
        static_assert(std::is_same_v<decltype(cc::begin(cc_ref)), int const*>);
        static_assert(std::is_same_v<decltype(cc::end(cc_ref)), int const*>);

        SUCCEED();
    }
}

TEST("utility - begin/end with C-style arrays")
{
    SECTION("int array")
    {
        int arr[5] = {1, 2, 3, 4, 5};

        CHECK(cc::begin(arr) == &arr[0]);
        CHECK(cc::end(arr) == &arr[0] + 5);
        CHECK(cc::end(arr) - cc::begin(arr) == 5);

        // can modify through begin
        *cc::begin(arr) = 100;
        CHECK(arr[0] == 100);
    }

    SECTION("const array")
    {
        int const arr[3] = {10, 20, 30};

        CHECK(cc::begin(arr) == &arr[0]);
        CHECK(cc::end(arr) == &arr[0] + 3);

        static_assert(std::is_same_v<decltype(cc::begin(arr)), int const*>);
        static_assert(std::is_same_v<decltype(cc::end(arr)), int const*>);
    }

    SECTION("char array")
    {
        char arr[4] = {'a', 'b', 'c', 'd'};

        CHECK(cc::begin(arr) == &arr[0]);
        CHECK(cc::end(arr) == &arr[0] + 4);
        CHECK(*cc::begin(arr) == 'a');
        CHECK(*(cc::end(arr) - 1) == 'd');
    }

    SECTION("single element array")
    {
        int arr[1] = {42};

        CHECK(cc::begin(arr) == &arr[0]);
        CHECK(cc::end(arr) == &arr[0] + 1);
        CHECK(cc::end(arr) - cc::begin(arr) == 1);
    }

    SECTION("iterate with begin/end")
    {
        int arr[4] = {1, 2, 3, 4};
        int sum = 0;

        for (int* it = cc::begin(arr); it != cc::end(arr); ++it)
        {
            sum += *it;
        }

        CHECK(sum == 10);
    }
}

TEST("utility - begin/end constexpr")
{
    SECTION("constexpr container begin/end")
    {
        struct constexpr_container
        {
            int value = 42;
            constexpr int const* begin() const { return &value; }
            constexpr int const* end() const { return &value + 1; }
        };

        // verify cc::begin/end are constexpr-callable
        constexpr auto test = []() constexpr
        {
            constexpr_container c{};
            auto b = cc::begin(c);
            auto e = cc::end(c);
            return e - b;
        }();

        static_assert(test == 1);
        SUCCEED();
    }
}
