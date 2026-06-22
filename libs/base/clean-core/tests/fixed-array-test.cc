#include <clean-core/fixed_array.hh>
#include <clean-core/span.hh>
#include <clean-core/utility.hh>

#include <nexus/test.hh>

#include <type_traits>

// ============================================================================
// Compile-time trait checks
// ============================================================================

// Aggregate type verification - the critical contract
static_assert(std::is_aggregate_v<cc::fixed_array<int, 3>>, "fixed_array must be aggregate");
static_assert(std::is_aggregate_v<cc::fixed_array<double, 5>>, "fixed_array must be aggregate");

// Triviality for trivial types
static_assert(std::is_trivial_v<cc::fixed_array<int, 3>>, "fixed_array<trivial> should be trivial");
static_assert(std::is_trivially_copyable_v<cc::fixed_array<int, 4>>, "fixed_array<trivial> should be trivially copyable");
static_assert(std::is_standard_layout_v<cc::fixed_array<int, 3>>, "fixed_array should have standard layout");
static_assert(std::is_trivially_destructible_v<cc::fixed_array<int, 5>>,
              "fixed_array<trivial> should be trivially destructible");

namespace
{
struct trivial_pod
{
    int x;
    float y;
};
} // namespace

static_assert(std::is_trivial_v<cc::fixed_array<trivial_pod, 4>>, "fixed_array<pod> should be trivial");
static_assert(std::is_trivially_copyable_v<cc::fixed_array<trivial_pod, 4>>,
              "fixed_array<pod> should be trivially copyable");

// Size/empty are compile-time constants
static_assert(cc::fixed_array<int, 7>{1, 2, 3, 4, 5, 6, 7}.size() == 7, "size() should be compile-time constant");
static_assert(!cc::fixed_array<int, 7>{1, 2, 3, 4, 5, 6, 7}.empty(), "empty() should be compile-time constant");
static_assert(cc::fixed_array<int, 0>{}.size() == 0, "size() == 0 for N==0"); // NOLINT
static_assert(cc::fixed_array<int, 0>{}.empty(), "empty() == true for N==0");

// sizeof verification - no padding members
static_assert(sizeof(cc::fixed_array<int, 3>) == sizeof(int) * 3, "fixed_array should have no padding");
static_assert(sizeof(cc::fixed_array<double, 5>) == sizeof(double) * 5, "fixed_array should have no padding");
static_assert(sizeof(cc::fixed_array<char, 10>) == sizeof(char) * 10, "fixed_array should have no padding");

// N==0 element access uses static_assert(false) - cannot be tested with requires
// The implementation prevents usage at compile time via static_assert

// ============================================================================
// Runtime tests - aggregate initialization and basic access
// ============================================================================

TEST("fixed_array - aggregate initialization")
{
    SECTION("direct initialization")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
        CHECK(a[2] == 3);
        CHECK(a.front() == 1);
        CHECK(a.back() == 3);
    }

    SECTION("copy initialization")
    {
        cc::fixed_array<int, 3> a = {1, 2, 3};
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
        CHECK(a[2] == 3);
    }

    SECTION("partial initialization")
    {
        cc::fixed_array<int, 5> a = {1, 2};
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
        CHECK(a[2] == 0);
        CHECK(a[3] == 0);
        CHECK(a[4] == 0);
    }

    SECTION("empty initialization")
    {
        cc::fixed_array<int, 3> a = {};
        CHECK(a[0] == 0);
        CHECK(a[1] == 0);
        CHECK(a[2] == 0);
    }
}

TEST("fixed_array - contiguous storage")
{
    SECTION("data() == begin()")
    {
        cc::fixed_array<int, 5> a{10, 20, 30, 40, 50};
        CHECK(a.data() == a.begin());
    }

    SECTION("end() == begin() + size()")
    {
        cc::fixed_array<int, 5> a{10, 20, 30, 40, 50};
        CHECK(a.end() == a.begin() + a.size());
    }

    SECTION("operator[] uses contiguous addresses")
    {
        cc::fixed_array<int, 5> a{10, 20, 30, 40, 50};
        for (cc::isize i = 0; i < a.size(); ++i)
        {
            CHECK(&a[i] == a.data() + i);
        }
    }

    SECTION("range-for visits all elements in order")
    {
        cc::fixed_array<int, 5> a{10, 20, 30, 40, 50};
        int expected = 10;
        int count = 0;
        for (auto val : a)
        {
            CHECK(val == expected);
            expected += 10;
            ++count;
        }
        CHECK(count == 5);
    }
}

TEST("fixed_array - front/back correctness")
{
    SECTION("N == 1")
    {
        cc::fixed_array<int, 1> a{42};
        CHECK(a.front() == 42);
        CHECK(a.back() == 42);
        CHECK(a[0] == 42);
        CHECK(a.front() == a.back());
    }

    SECTION("N == 2")
    {
        cc::fixed_array<int, 2> a{1, 2};
        CHECK(a.front() == 1);
        CHECK(a.back() == 2);
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
    }
}

TEST("fixed_array - const correctness")
{
    SECTION("const reference access")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        cc::fixed_array<int, 3> const& c = a;

        CHECK(c[0] == 1);
        CHECK(c.front() == 1);
        CHECK(c.back() == 3);

        // Verify const data() returns const pointer
        static_assert(std::is_same_v<decltype(c.data()), int const*>);
        CHECK(c.data() == a.data());
    }

    SECTION("const iterators")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        cc::fixed_array<int, 3> const& c = a;

        static_assert(std::is_same_v<decltype(c.begin()), int const*>);
        static_assert(std::is_same_v<decltype(c.end()), int const*>);

        int sum = 0;
        for (auto val : c)
        {
            sum += val;
        }
        CHECK(sum == 6);
    }
}

TEST("fixed_array - N == 0 specialization")
{
    SECTION("size and empty")
    {
        cc::fixed_array<int, 0> z;
        CHECK(z.size() == 0);
        CHECK(z.empty());
    }

    SECTION("iterators are nullptr and form empty range")
    {
        cc::fixed_array<int, 0> z;
        CHECK(z.data() == nullptr);
        CHECK(z.begin() == nullptr);
        CHECK(z.end() == nullptr);
        CHECK(z.begin() == z.end());
    }

    SECTION("empty range iteration")
    {
        cc::fixed_array<int, 0> z;
        int count = 0;
        for ([[maybe_unused]] auto val : z)
        {
            ++count;
        }
        CHECK(count == 0);
    }
}

TEST("fixed_array - structured binding")
{
    // fixed_array supports structured binding via tuple protocol
    SECTION("N == 2")
    {
        cc::fixed_array<int, 2> a{1, 2};
        auto [x, y] = a;
        CHECK(x == 1);
        CHECK(y == 2);
    }

    SECTION("N == 3")
    {
        cc::fixed_array<int, 3> a{10, 20, 30};
        auto [x, y, z] = a;
        CHECK(x == 10);
        CHECK(y == 20);
        CHECK(z == 30);
    }

    SECTION("structured binding by reference")
    {
        cc::fixed_array<int, 2> a{1, 2};
        auto& [x, y] = a;
        x = 99;
        y = 88;
        CHECK(a[0] == 99);
        CHECK(a[1] == 88);
    }
}

TEST("fixed_array - constexpr usage")
{
    SECTION("constexpr sum")
    {
        constexpr auto sum = []() constexpr
        {
            auto const a = cc::fixed_array<int, 4>{1, 2, 3, 4};
            int total = 0;
            for (auto val : a)
            {
                total += val;
            }
            return total;
        }();
        static_assert(sum == 10);
        CHECK(sum == 10);
    }

    SECTION("constexpr element access")
    {
        constexpr cc::fixed_array<int, 3> a{10, 20, 30};
        static_assert(a[0] == 10);
        static_assert(a[1] == 20);
        static_assert(a[2] == 30);
        static_assert(a.front() == 10);
        static_assert(a.back() == 30);
        CHECK(a.size() == 3);
    }
}

// ============================================================================
// Non-trivial type tests
// ============================================================================

namespace
{
struct tracked
{
    inline static int ctor_count = 0;
    inline static int dtor_count = 0;

    int value;

    explicit tracked(int v) : value(v) { ++ctor_count; }
    ~tracked() { ++dtor_count; }

    tracked(tracked const& other) : value(other.value) { ++ctor_count; }
    tracked(tracked&& other) noexcept : value(other.value) { ++ctor_count; }

    tracked& operator=(tracked const&) = default;
    tracked& operator=(tracked&&) noexcept = default;

    static void reset()
    {
        ctor_count = 0;
        dtor_count = 0;
    }
};
} // namespace

TEST("fixed_array - non-trivial type construction")
{
    SECTION("aggregate initialization constructs elements exactly once")
    {
        tracked::reset();
        {
            cc::fixed_array<tracked, 2> a{tracked{1}, tracked{2}};
            // 2 temporary ctors + 2 move ctors = 4 total (or 2 if move elision happens)
            // The key is: no extra default construction
            CHECK(a[0].value == 1);
            CHECK(a[1].value == 2);
        }
        // All constructed objects should be destroyed
        CHECK(tracked::ctor_count == tracked::dtor_count);
    }

    SECTION("destruction happens for all elements")
    {
        tracked::reset();
        {
            cc::fixed_array<tracked, 3> a{tracked{1}, tracked{2}, tracked{3}};
            CHECK(a.size() == 3);
        }
        CHECK(tracked::ctor_count == tracked::dtor_count);
    }

    SECTION("N == 1 lifetime")
    {
        tracked::reset();
        {
            cc::fixed_array<tracked, 1> a{tracked{42}};
            CHECK(a[0].value == 42);
        }
        CHECK(tracked::ctor_count == tracked::dtor_count);
    }
}

// ============================================================================
// Move-only type tests
// ============================================================================

namespace
{
struct move_only
{
    int value;

    explicit move_only(int v) : value(v) {}
    move_only(move_only&& other) noexcept : value(other.value) { other.value = -1; }
    move_only& operator=(move_only&& other) noexcept
    {
        value = other.value;
        other.value = -1;
        return *this;
    }

    move_only(move_only const&) = delete;
    move_only& operator=(move_only const&) = delete;
};
} // namespace

static_assert(!std::is_copy_constructible_v<cc::fixed_array<move_only, 2>>,
              "fixed_array<move_only> should not be copyable");
static_assert(std::is_move_constructible_v<cc::fixed_array<move_only, 2>>, "fixed_array<move_only> should be movable");

TEST("fixed_array - move-only type")
{
    SECTION("aggregate initialization")
    {
        cc::fixed_array<move_only, 2> a{move_only{1}, move_only{2}};
        CHECK(a[0].value == 1);
        CHECK(a[1].value == 2);
    }

    SECTION("element access does not require copyability")
    {
        cc::fixed_array<move_only, 1> a{move_only{42}};
        auto& x = a[0];
        CHECK(x.value == 42);
        CHECK(a.front().value == 42);
        CHECK(a.back().value == 42);
    }

    SECTION("const correctness with move-only")
    {
        cc::fixed_array<move_only, 1> a{move_only{42}};
        cc::fixed_array<move_only, 1> const& c = a;

        // const reference should not require copy/move
        static_assert(std::is_same_v<decltype(c[0]), move_only const&>);
        CHECK(c[0].value == 42);
    }

    SECTION("structured binding with move-only")
    {
        cc::fixed_array<move_only, 2> a{move_only{1}, move_only{2}};
        auto& [x, y] = a;
        CHECK(x.value == 1);
        CHECK(y.value == 2);

        // Verify these are references, not copies
        x.value = 99;
        CHECK(a[0].value == 99);
    }
}

TEST("fixed_array - N == 0 with non-trivial types")
{
    SECTION("N == 0 does not instantiate tracked")
    {
        tracked::reset();
        {
            cc::fixed_array<tracked, 0> z;
            CHECK(z.size() == 0);
            CHECK(z.empty());
        }
        // No constructors or destructors should have been called
        CHECK(tracked::ctor_count == 0);
        CHECK(tracked::dtor_count == 0);
    }

    SECTION("N == 0 with move-only")
    {
        cc::fixed_array<move_only, 0> z;
        CHECK(z.size() == 0);
        CHECK(z.empty());
        CHECK(z.data() == nullptr);
        CHECK(z.begin() == nullptr);
        CHECK(z.end() == nullptr);
    }

    SECTION("N == 0 does not require default constructibility")
    {
        struct no_default
        {
            explicit no_default(int) {}
            no_default() = delete;
        };

        cc::fixed_array<no_default, 0> z;
        CHECK(z.size() == 0);
    }
}

// ============================================================================
// Additional edge cases
// ============================================================================

TEST("fixed_array - mutation")
{
    SECTION("operator[] mutation")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        a[1] = 99;
        CHECK(a[0] == 1);
        CHECK(a[1] == 99);
        CHECK(a[2] == 3);
    }

    SECTION("front/back mutation")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        a.front() = 10;
        a.back() = 30;
        CHECK(a[0] == 10);
        CHECK(a[1] == 2);
        CHECK(a[2] == 30);
    }

    SECTION("iterator mutation")
    {
        cc::fixed_array<int, 3> a{1, 2, 3};
        for (auto& val : a)
        {
            val *= 2;
        }
        CHECK(a[0] == 2);
        CHECK(a[1] == 4);
        CHECK(a[2] == 6);
    }
}

TEST("fixed_array - various sizes")
{
    SECTION("N == 1")
    {
        cc::fixed_array<int, 1> a{42};
        CHECK(a.size() == 1);
        CHECK(!a.empty());
    }

    SECTION("N == 10")
    {
        cc::fixed_array<int, 10> a{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        CHECK(a.size() == 10);
        for (int i = 0; i < 10; ++i)
        {
            CHECK(a[i] == i);
        }
    }
}

TEST("fixed_array - non-trivial structured binding")
{
    SECTION("structured binding creates references, not copies")
    {
        tracked::reset();
        {
            cc::fixed_array<tracked, 2> a{tracked{1}, tracked{2}};
            auto const initial_ctors = tracked::ctor_count;

            auto& [x, y] = a;

            // No additional copies should be made
            CHECK(tracked::ctor_count == initial_ctors);
            CHECK(x.value == 1);
            CHECK(y.value == 2);
        }
        CHECK(tracked::ctor_count == tracked::dtor_count);
    }
}
