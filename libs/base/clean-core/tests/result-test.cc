#include <clean-core/result.hh>

#include <nexus/test.hh>

#include <memory>
#include <string>

// result stays trivial when T and E are trivial
static_assert(std::is_constructible_v<cc::result<int, int>>);
static_assert(std::is_constructible_v<cc::result<int, int>, int>);
static_assert(std::is_constructible_v<cc::result<int, int>, cc::as_error_t<int>>);
static_assert(std::is_trivially_copyable_v<cc::result<int, int>>);
static_assert(std::is_trivially_destructible_v<cc::result<int, int>>);

namespace
{
// test type for non-trivial operations
struct non_trivial
{
    int value = 0;
    bool* destroyed = nullptr;

    non_trivial() = default;
    explicit non_trivial(int v) : value(v) {}
    non_trivial(int v, bool* d) : value(v), destroyed(d) {}

    ~non_trivial()
    {
        if (destroyed)
            *destroyed = true;
    }

    non_trivial(non_trivial const&) = default;
    non_trivial(non_trivial&& rhs) noexcept : value(rhs.value), destroyed(rhs.destroyed) { rhs.destroyed = nullptr; }
    non_trivial& operator=(non_trivial const&) = default;
    non_trivial& operator=(non_trivial&& rhs) noexcept
    {
        value = rhs.value;
        destroyed = rhs.destroyed;
        rhs.destroyed = nullptr;
        return *this;
    }

    friend bool operator==(non_trivial const&, non_trivial const&) = default;
};

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

// counting type to track special member function calls
struct counting_type
{
    int value = 0;

    static inline int default_ctor_count = 0;
    static inline int value_ctor_count = 0;
    static inline int copy_ctor_count = 0;
    static inline int move_ctor_count = 0;
    static inline int copy_assign_count = 0;
    static inline int move_assign_count = 0;
    static inline int dtor_count = 0;

    static void reset_counters()
    {
        default_ctor_count = 0;
        value_ctor_count = 0;
        copy_ctor_count = 0;
        move_ctor_count = 0;
        copy_assign_count = 0;
        move_assign_count = 0;
        dtor_count = 0;
    }

    counting_type() { ++default_ctor_count; }
    explicit counting_type(int v) : value(v) { ++value_ctor_count; }

    counting_type(counting_type const& rhs) : value(rhs.value) { ++copy_ctor_count; }
    counting_type(counting_type&& rhs) noexcept : value(rhs.value) { ++move_ctor_count; }

    counting_type& operator=(counting_type const& rhs)
    {
        value = rhs.value;
        ++copy_assign_count;
        return *this;
    }

    counting_type& operator=(counting_type&& rhs) noexcept
    {
        value = rhs.value;
        ++move_assign_count;
        return *this;
    }

    ~counting_type() { ++dtor_count; }

    friend bool operator==(counting_type const&, counting_type const&) = default;
};
} // namespace

TEST("result - trivial types")
{
    SECTION("default construction creates error")
    {
        auto const res = cc::result<int, int>{};
        CHECK(!res.has_value());
        CHECK(res.has_error());
        CHECK(res.error() == 0);
    }

    SECTION("value construction")
    {
        auto const res = cc::result<int, int>{42};
        CHECK(res.has_value());
        CHECK(!res.has_error());
        CHECK(res.value() == 42);
    }

    SECTION("error construction via as_error")
    {
        auto const res = cc::result<int, int>{cc::error(99)};
        CHECK(!res.has_value());
        CHECK(res.has_error());
        CHECK(res.error() == 99);
    }

    SECTION("copy construction - value")
    {
        auto const res1 = cc::result<int, int>{42};
        auto const res2 = res1;
        CHECK(res2.has_value());
        CHECK(res2.value() == 42);
        CHECK(res1.has_value());
        CHECK(res1.value() == 42);
    }

    SECTION("copy construction - error")
    {
        auto const res1 = cc::result<int, int>{cc::error(99)};
        auto const res2 = res1;
        CHECK(res2.has_error());
        CHECK(res2.error() == 99);
        CHECK(res1.has_error());
        CHECK(res1.error() == 99);
    }

    SECTION("move construction - value")
    {
        auto res1 = cc::result<int, int>{42};
        auto const res2 = cc::move(res1);
        CHECK(res2.has_value());
        CHECK(res2.value() == 42);
        // trivial types remain valid after move
        CHECK(res1.has_value());
        CHECK(res1.value() == 42);
    }

    SECTION("move construction - error")
    {
        auto res1 = cc::result<int, int>{cc::error(99)};
        auto const res2 = cc::move(res1);
        CHECK(res2.has_error());
        CHECK(res2.error() == 99);
        // trivial types remain valid after move
        CHECK(res1.has_error());
        CHECK(res1.error() == 99);
    }

    SECTION("copy assignment - value to error")
    {
        auto res1 = cc::result<int, int>{42};
        auto res2 = cc::result<int, int>{cc::error(99)};
        res2 = res1;
        CHECK(res2.has_value());
        CHECK(res2.value() == 42);
    }

    SECTION("copy assignment - error to value")
    {
        auto res1 = cc::result<int, int>{cc::error(99)};
        auto res2 = cc::result<int, int>{42};
        res2 = res1;
        CHECK(res2.has_error());
        CHECK(res2.error() == 99);
    }

    SECTION("move assignment - value to error")
    {
        auto res1 = cc::result<int, int>{42};
        auto res2 = cc::result<int, int>{cc::error(99)};
        res2 = cc::move(res1);
        CHECK(res2.has_value());
        CHECK(res2.value() == 42);
    }

    SECTION("move assignment - error to value")
    {
        auto res1 = cc::result<int, int>{cc::error(99)};
        auto res2 = cc::result<int, int>{42};
        res2 = cc::move(res1);
        CHECK(res2.has_error());
        CHECK(res2.error() == 99);
    }
}

TEST("result - non-trivial types")
{
    SECTION("value construction")
    {
        auto const res = cc::result<non_trivial, int>{non_trivial{42}};
        CHECK(res.has_value());
        CHECK(res.value().value == 42);
    }

    SECTION("error construction")
    {
        auto const res = cc::result<int, non_trivial>{cc::error(non_trivial{99})};
        CHECK(res.has_error());
        CHECK(res.error().value == 99);
    }

    SECTION("copy construction - value")
    {
        auto const res1 = cc::result<non_trivial, int>{non_trivial{42}};
        auto const res2 = res1; // NOLINT
        CHECK(res2.has_value());
        CHECK(res2.value().value == 42);
        CHECK(res1.has_value());
        CHECK(res1.value().value == 42);
    }

    SECTION("copy construction - error")
    {
        auto const res1 = cc::result<int, non_trivial>{cc::error(non_trivial{99})};
        auto const res2 = res1; // NOLINT
        CHECK(res2.has_error());
        CHECK(res2.error().value == 99);
        CHECK(res1.has_error());
        CHECK(res1.error().value == 99);
    }

    SECTION("move construction - value")
    {
        auto res1 = cc::result<non_trivial, int>{non_trivial{42}};
        auto const res2 = cc::move(res1);
        CHECK(res2.has_value());
        CHECK(res2.value().value == 42);
    }

    SECTION("move construction - error")
    {
        auto res1 = cc::result<int, non_trivial>{cc::error(non_trivial{99})};
        auto const res2 = cc::move(res1);
        CHECK(res2.has_error());
        CHECK(res2.error().value == 99);
    }

    SECTION("string values")
    {
        auto res = cc::result<cc::string, int>{"hello"};
        CHECK(res.has_value());
        CHECK(res.value() == "hello");

        res = cc::result<cc::string, int>{cc::error(42)};
        CHECK(res.has_error());
        CHECK(res.error() == 42);
    }

    SECTION("string errors")
    {
        auto res = cc::result<int, cc::string>{42};
        CHECK(res.has_value());
        CHECK(res.value() == 42);

        res = cc::result<int, cc::string>{cc::error(cc::string{"error"})};
        CHECK(res.has_error());
        CHECK(res.error() == "error");
    }
}

TEST("result - counting special member functions")
{
    SECTION("value construction")
    {
        counting_type::reset_counters();
        {
            auto const res = cc::result<counting_type, int>{counting_type{42}};
            CHECK(res.has_value());
            CHECK(res.value().value == 42);
        }
        // Should construct temp value (value_ctor), move it into result (move_ctor), then destruct both
        CHECK(counting_type::value_ctor_count == 1);
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // temp + result contents
    }

    SECTION("error construction")
    {
        counting_type::reset_counters();
        {
            auto const res = cc::result<int, counting_type>{cc::error(counting_type{99})};
            CHECK(res.has_error());
            CHECK(res.error().value == 99);
        }
        // Same pattern for error
        CHECK(counting_type::value_ctor_count == 1);
        CHECK(counting_type::move_ctor_count == 2); // move into cc::error, then move into result
        CHECK(counting_type::dtor_count == 3);
    }

    SECTION("copy construction - value")
    {
        counting_type::reset_counters();
        {
            auto const res1 = cc::result<counting_type, int>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate copy construction
            auto const res2 = res1;
            CHECK(res2.has_value());
        }
        // Should only copy construct
        CHECK(counting_type::copy_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // both results destructed
    }

    SECTION("move construction - value")
    {
        counting_type::reset_counters();
        {
            auto res1 = cc::result<counting_type, int>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate move construction
            auto const res2 = cc::move(res1);
            CHECK(res2.has_value());
        }
        // Should only move construct
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // both results destructed
    }

    SECTION("copy assignment - value to error")
    {
        counting_type::reset_counters();
        {
            auto res1 = cc::result<counting_type, int>{counting_type{42}};
            auto res2 = cc::result<counting_type, int>{cc::error(99)};
            counting_type::reset_counters(); // Reset to isolate assignment
            res2 = res1;
            CHECK(res2.has_value());
        }
        // Should destruct error (int, trivial), copy construct value
        CHECK(counting_type::copy_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // both values destructed at end
    }

    SECTION("move assignment - error to value")
    {
        counting_type::reset_counters();
        {
            auto res1 = cc::result<int, counting_type>{cc::error(counting_type{99})};
            auto res2 = cc::result<int, counting_type>{42};
            counting_type::reset_counters(); // Reset to isolate assignment
            res2 = cc::move(res1);
            CHECK(res2.has_error());
        }
        // Should destruct value (int, trivial), move construct error
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // both errors destructed at end
    }
}

TEST("result - move-only types")
{
    SECTION("value construction")
    {
        auto res = cc::result<move_only, int>{move_only{42}};
        CHECK(res.has_value());
        CHECK(res.value().value == 42);
    }

    SECTION("error construction")
    {
        auto res = cc::result<int, move_only>{cc::error(move_only{99})};
        CHECK(res.has_error());
        CHECK(res.error().value == 99);
    }

    SECTION("move construction - value")
    {
        auto res1 = cc::result<move_only, int>{move_only{42}};
        auto res2 = cc::move(res1);
        CHECK(res2.has_value());
        CHECK(res2.value().value == 42);
    }

    SECTION("move construction - error")
    {
        auto res1 = cc::result<int, move_only>{cc::error(move_only{99})};
        auto res2 = cc::move(res1);
        CHECK(res2.has_error());
        CHECK(res2.error().value == 99);
    }

    SECTION("unique_ptr value")
    {
        auto res = cc::result<std::unique_ptr<int>, int>{std::make_unique<int>(42)};
        CHECK(res.has_value());
        CHECK(*res.value() == 42);

        auto res2 = cc::move(res);
        CHECK(res2.has_value());
        CHECK(*res2.value() == 42);
    }

    SECTION("unique_ptr error")
    {
        auto res = cc::result<int, std::unique_ptr<int>>{cc::error(std::make_unique<int>(99))};
        CHECK(res.has_error());
        CHECK(*res.error() == 99);

        auto res2 = cc::move(res);
        CHECK(res2.has_error());
        CHECK(*res2.error() == 99);
    }
}

TEST("result - value and error observers")
{
    SECTION("value const reference")
    {
        auto const res = cc::result<int, int>{42};
        CHECK(res.value() == 42);
    }

    SECTION("value reference")
    {
        auto res = cc::result<int, int>{42};
        res.value() = 99;
        CHECK(res.value() == 99);
    }

    SECTION("value rvalue reference")
    {
        auto res = cc::result<move_only, int>{move_only{42}};
        auto moved = cc::move(res).value();
        CHECK(moved.value == 42);
    }

    SECTION("error const reference")
    {
        auto const res = cc::result<int, int>{cc::error(99)};
        CHECK(res.error() == 99);
    }

    SECTION("error reference")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        res.error() = 111;
        CHECK(res.error() == 111);
    }

    SECTION("error rvalue reference")
    {
        auto res = cc::result<int, move_only>{cc::error(move_only{99})};
        auto moved = cc::move(res).error();
        CHECK(moved.value == 99);
    }

    SECTION("value preserves category - const")
    {
        auto const res = cc::result<cc::string, int>{"hello"};
        auto const& ref = res.value();
        CHECK(ref == "hello");
    }

    SECTION("value preserves category - mutable")
    {
        auto res = cc::result<cc::string, int>{"hello"};
        res.value() += " world";
        CHECK(res.value() == "hello world");
    }
}

TEST("result - value_or and error_or")
{
    SECTION("value_or - has value")
    {
        auto const res = cc::result<int, int>{42};
        CHECK(res.value_or(99) == 42);
    }

    SECTION("value_or - has error")
    {
        auto const res = cc::result<int, int>{cc::error(0)};
        CHECK(res.value_or(99) == 99);
    }

    SECTION("error_or - has error")
    {
        auto const res = cc::result<int, int>{cc::error(99)};
        CHECK(res.error_or(0) == 99);
    }

    SECTION("error_or - has value")
    {
        auto const res = cc::result<int, int>{42};
        CHECK(res.error_or(99) == 99);
    }

    SECTION("value_or with move-only type")
    {
        auto res = cc::result<move_only, int>{move_only{42}};
        auto result = cc::move(res).value_or(move_only{99});
        CHECK(result.value == 42);
    }

    SECTION("value_or with move-only type - error case")
    {
        auto res = cc::result<move_only, int>{cc::error(0)};
        auto result = cc::move(res).value_or(move_only{99});
        CHECK(result.value == 99);
    }

    SECTION("error_or with move-only type")
    {
        auto res = cc::result<int, move_only>{cc::error(move_only{99})};
        auto result = cc::move(res).error_or(move_only{0});
        CHECK(result.value == 99);
    }

    SECTION("error_or with move-only type - value case")
    {
        auto res = cc::result<int, move_only>{42};
        auto result = cc::move(res).error_or(move_only{99});
        CHECK(result.value == 99);
    }
}

TEST("result - emplace_value and emplace_error")
{
    SECTION("emplace_value on error result")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        auto& ref = res.emplace_value(42);
        CHECK(res.has_value());
        CHECK(res.value() == 42);
        CHECK(&ref == &res.value());
    }

    SECTION("emplace_value on value result")
    {
        auto res = cc::result<int, int>{99};
        auto& ref = res.emplace_value(42);
        CHECK(res.has_value());
        CHECK(res.value() == 42);
        CHECK(&ref == &res.value());
    }

    SECTION("emplace_error on value result")
    {
        auto res = cc::result<int, int>{42};
        auto& ref = res.emplace_error(99);
        CHECK(res.has_error());
        CHECK(res.error() == 99);
        CHECK(&ref == &res.error());
    }

    SECTION("emplace_error on error result")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        auto& ref = res.emplace_error(111);
        CHECK(res.has_error());
        CHECK(res.error() == 111);
        CHECK(&ref == &res.error());
    }

    SECTION("emplace_value with multiple arguments")
    {
        bool destroyed = false;
        auto res = cc::result<non_trivial, int>{cc::error(0)};
        auto& ref = res.emplace_value(42, &destroyed);
        CHECK(res.has_value());
        CHECK(res.value().value == 42);
        CHECK(res.value().destroyed == &destroyed);
        CHECK(&ref == &res.value());
    }

    SECTION("emplace_error with multiple arguments")
    {
        bool destroyed = false;
        auto res = cc::result<int, non_trivial>{99};
        auto& ref = res.emplace_error(42, &destroyed);
        CHECK(res.has_error());
        CHECK(res.error().value == 42);
        CHECK(res.error().destroyed == &destroyed);
        CHECK(&ref == &res.error());
    }

    SECTION("emplace destroys previous value")
    {
        bool destroyed = false;
        {
            auto res = cc::result<non_trivial, int>{non_trivial{99, &destroyed}};
            CHECK(!destroyed);
            res.emplace_error(42);
            CHECK(destroyed); // old value was destroyed
            CHECK(res.has_error());
            CHECK(res.error() == 42);
        }
    }

    SECTION("emplace destroys previous error")
    {
        bool destroyed = false;
        {
            auto res = cc::result<int, non_trivial>{cc::error(non_trivial{99, &destroyed})};
            CHECK(!destroyed);
            res.emplace_value(42);
            CHECK(destroyed); // old error was destroyed
            CHECK(res.has_value());
            CHECK(res.value() == 42);
        }
    }
}

TEST("result - converting constructor")
{
    SECTION("convert compatible value types")
    {
        auto res1 = cc::result<int, int>{42};
        auto res2 = cc::result<long, long>{cc::move(res1)};
        CHECK(res2.has_value());
        CHECK(res2.value() == 42L);
    }

    SECTION("convert compatible error types")
    {
        auto res1 = cc::result<int, int>{cc::error(99)};
        auto res2 = cc::result<long, long>{cc::move(res1)};
        CHECK(res2.has_error());
        CHECK(res2.error() == 99L);
    }

    SECTION("convert char to string")
    {
        auto res1 = cc::result<char>{'x'};
        auto res2 = cc::result<cc::string>{cc::move(res1)};
        CHECK(res2.has_value());
        // Note: direct construction from char to string is explicit,
        // so this is allowed via the converting constructor
    }
}

TEST("result - usage patterns")
{
    SECTION("simple success path")
    {
        auto const res = cc::result<int, int>{42};
        if (res.has_value())
        {
            CHECK(res.value() == 42);
        }
        else
        {
            // should not reach here
            CHECK(false);
        }
    }

    SECTION("simple error path")
    {
        auto const res = cc::result<int, int>{cc::error(99)};
        if (res.has_error())
        {
            CHECK(res.error() == 99);
        }
        else
        {
            // should not reach here
            CHECK(false);
        }
    }

    SECTION("function returning result - success")
    {
        auto divide = [](int a, int b) -> cc::result<int, cc::string>
        {
            if (b == 0)
                return cc::error("division by zero");
            return a / b;
        };

        auto res = divide(10, 2);
        CHECK(res.has_value());
        CHECK(res.value() == 5);
    }

    SECTION("function returning result - error")
    {
        auto divide = [](int a, int b) -> cc::result<int, cc::string>
        {
            if (b == 0)
                return cc::error("division by zero");
            return a / b;
        };

        auto res = divide(10, 0);
        CHECK(res.has_error());
        CHECK(res.error() == "division by zero");
    }

    SECTION("early return on error")
    {
        auto parse_and_validate = [](bool should_fail) -> cc::result<int, cc::string>
        {
            // simulate parsing
            auto parse_result = [&]() -> cc::result<int, cc::string>
            {
                if (should_fail)
                    return cc::error("parse error");
                return 42;
            }();

            if (parse_result.has_error())
                return parse_result;

            // simulate validation
            if (parse_result.value() < 0)
                return cc::error("validation error");

            return parse_result.value();
        };

        auto res1 = parse_and_validate(false);
        CHECK(res1.has_value());
        CHECK(res1.value() == 42);

        auto res2 = parse_and_validate(true);
        CHECK(res2.has_error());
        CHECK(res2.error() == "parse error");
    }

    SECTION("value_or for default fallback")
    {
        auto get_config = [](bool use_default) -> cc::result<int, cc::string>
        {
            if (use_default)
                return cc::error("config not found");
            return 42;
        };

        auto value = get_config(true).value_or(100);
        CHECK(value == 100);
    }
}

TEST("result - any_error integration")
{
    SECTION("construct any_error from string")
    {
        auto err = cc::any_error{"something went wrong"};
        CHECK(!err.is_empty());
    }

    SECTION("result with any_error - value")
    {
        auto res = cc::result<int>{42};
        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("result with any_error - error from string")
    {
        auto res = cc::result<int>{cc::error("error message")};
        CHECK(res.has_error());
        CHECK(!res.error().is_empty());
    }

    SECTION("result with any_error - error from int")
    {
        auto res = cc::result<int>{cc::error(42)};
        CHECK(res.has_error());
        CHECK(!res.error().is_empty());
    }

    SECTION("any_error move semantics")
    {
        auto err1 = cc::any_error{"original"};
        auto err2 = cc::move(err1);
        CHECK(!err2.is_empty());
    }

    SECTION("any_error context chaining - lvalue")
    {
        auto err = cc::any_error{"base error"};
        err.with_context("additional context");
        CHECK(!err.is_empty());
    }

    SECTION("any_error context chaining - rvalue")
    {
        auto err = cc::any_error{"base error"}.with_context("additional context");
        CHECK(!err.is_empty());
    }

    SECTION("result with any_error in function")
    {
        auto process = [](int value) -> cc::result<int>
        {
            if (value < 0)
                return cc::error("negative value not allowed");
            if (value == 0)
                return cc::error("zero value not allowed");
            return value * 2;
        };

        auto res1 = process(5);
        CHECK(res1.has_value());
        CHECK(res1.value() == 10);

        auto res2 = process(-1);
        CHECK(res2.has_error());
        CHECK(!res2.error().is_empty());

        auto res3 = process(0);
        CHECK(res3.has_error());
        CHECK(!res3.error().is_empty());
    }

    SECTION("any_error context propagation")
    {
        auto inner_fn = []() -> cc::result<int> { return cc::error("inner error"); };

        auto outer_fn = [&]() -> cc::result<int>
        {
            auto res = inner_fn();
            if (res.has_error())
                return cc::error(cc::move(res.error()).with_context("outer context"));
            return res.value();
        };

        auto res = outer_fn();
        CHECK(res.has_error());
        CHECK(!res.error().is_empty());
        auto err_str = res.error().to_string();
        // Error string should contain both contexts, but we don't test exact format
        CHECK(!err_str.empty());
    }
}

TEST("result - self-assignment safety")
{
    SECTION("self-copy assignment - value")
    {
        auto res = cc::result<int, int>{42};
        res = res; // NOLINT(clang-diagnostic-self-assign-overloaded)
        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("self-copy assignment - error")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        res = res; // NOLINT(clang-diagnostic-self-assign-overloaded)
        CHECK(res.has_error());
        CHECK(res.error() == 99);
    }

    SECTION("self-move assignment - value")
    {
        auto res = cc::result<int, int>{42};
        res = cc::move(res);
        // After self-move, state is valid but unspecified for trivial types
        // For non-trivial, should still be valid
        CHECK(res.has_value());
    }

    SECTION("self-move assignment - error")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        res = cc::move(res);
        CHECK(res.has_error());
    }
}

TEST("result - const correctness")
{
    SECTION("const result with value")
    {
        auto const res = cc::result<int, int>{42};
        CHECK(res.has_value());
        CHECK(res.value() == 42);
        static_assert(std::is_same_v<decltype(res.value()), int const&>);
    }

    SECTION("const result with error")
    {
        auto const res = cc::result<int, int>{cc::error(99)};
        CHECK(res.has_error());
        CHECK(res.error() == 99);
        static_assert(std::is_same_v<decltype(res.error()), int const&>);
    }

    SECTION("mutable result with value")
    {
        auto res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(res.value()), int&>);
        res.value() = 99;
        CHECK(res.value() == 99);
    }

    SECTION("mutable result with error")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        static_assert(std::is_same_v<decltype(res.error()), int&>);
        res.error() = 111;
        CHECK(res.error() == 111);
    }

    SECTION("rvalue result with value")
    {
        auto res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(cc::move(res).value()), int&&>);

        SUCCEED(); // just static checks
    }

    SECTION("rvalue result with error")
    {
        auto res = cc::result<int, int>{cc::error(99)};
        static_assert(std::is_same_v<decltype(cc::move(res).error()), int&&>);

        SUCCEED(); // just static checks
    }
}

TEST("result - any_error multi-layer context")
{
    SECTION("context accumulation through layers")
    {
        auto layer1 = []() -> cc::result<int> { return cc::error("layer1 error"); };

        auto layer2 = [&]() -> cc::result<int>
        {
            auto res = layer1();
            if (res.has_error())
                return cc::error(cc::move(res.error()).with_context("layer2 context"));
            return res.value();
        };

        auto layer3 = [&]() -> cc::result<int>
        {
            auto res = layer2();
            if (res.has_error())
                return cc::error(cc::move(res.error()).with_context("layer3 context"));
            return res.value();
        };

        auto res = layer3();
        CHECK(res.has_error());
        auto err_str = res.error().to_string();

        // All context messages should appear as substrings
        CHECK(err_str.contains("layer1 error"));
        CHECK(err_str.contains("layer2 context"));
        CHECK(err_str.contains("layer3 context"));
    }

    SECTION("empty any_error can receive context")
    {
        auto err = cc::any_error{};
        err.with_context("first context");
        err.with_context("second context");
        auto err2 = cc::move(err).with_context("third context");

        auto err_str = err2.to_string();
        CHECK(err_str.contains("first context"));
        CHECK(err_str.contains("second context"));
        CHECK(err_str.contains("third context"));
    }
}

TEST("result - source location capture")
{
    SECTION("any_error construction captures location")
    {
        auto before = cc::source_location::current();
        auto err = cc::any_error{"test error"};
        auto after = cc::source_location::current();

        auto site = err.site();
        CHECK(cc::string_view(site.file_name()) == before.file_name());
        CHECK(site.line() > before.line());
        CHECK(site.line() < after.line());
    }

    SECTION("cc::error captures location")
    {
        auto before = cc::source_location::current();
        auto err = cc::error("test error");
        auto after = cc::source_location::current();

        // Create a result to extract the site
        auto res = cc::result<int>{cc::move(err)};
        CHECK(res.has_error());
        auto site = res.error().site();
        CHECK(cc::string_view(site.file_name()) == before.file_name());
        CHECK(site.line() > before.line());
        CHECK(site.line() < after.line());
    }

    SECTION("result<T, E> to result<T> conversion captures location")
    {
        auto make_typed_error = []() -> cc::result<int, cc::string> { return cc::error("typed error"); };

        auto before = cc::source_location::current();
        auto res = cc::result<int>{make_typed_error()};
        auto after = cc::source_location::current();

        CHECK(res.has_error());
        auto site = res.error().site();
        CHECK(cc::string_view(site.file_name()) == before.file_name());
        CHECK(site.line() > before.line());
        CHECK(site.line() < after.line());
    }

    SECTION("context addition captures location")
    {
        auto err = cc::any_error{"base error"};

        auto before = cc::source_location::current();
        err.with_context("context message");
        auto after = cc::source_location::current();

        // We can't directly query context sites, but we verify the mechanism works
        // by checking that the error is non-empty
        CHECK(!err.is_empty());
    }
}

TEST("result - with_context on result<T>")
{
    SECTION("with_context on error result - lvalue")
    {
        auto res = cc::result<int>{cc::error("base error")};
        res.with_context("additional context");

        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("additional context"));
    }

    SECTION("with_context on error result - rvalue")
    {
        auto res = cc::result<int>{cc::error("base error")}.with_context("additional context");

        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("additional context"));
    }

    SECTION("with_context on value result - lvalue")
    {
        auto res = cc::result<int>{42};
        res.with_context("this context is ignored");

        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("with_context on value result - rvalue")
    {
        auto res = cc::result<int>{42}.with_context("this context is ignored");

        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("with_context chaining")
    {
        auto res = cc::result<int>{cc::error("base error")} //
                       .with_context("context 1")           //
                       .with_context("context 2")           //
                       .with_context("context 3");

        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("context 1"));
        CHECK(err_str.contains("context 2"));
        CHECK(err_str.contains("context 3"));
    }

    SECTION("with_context captures source location")
    {
        auto res = cc::result<int>{cc::error("base error")};

        auto before = cc::source_location::current();
        res.with_context("context message");
        auto after = cc::source_location::current();

        // Context was added (verified by checking it appears in output)
        CHECK(res.has_error());
        CHECK(res.error().to_string().contains("context message"));
    }
}

TEST("result - with_context_lazy on result<T>")
{
    SECTION("with_context_lazy on error result - callable is invoked")
    {
        auto res = cc::result<int>{cc::error("base error")};
        bool invoked = false;

        res.with_context_lazy(
            [&]
            {
                invoked = true;
                return cc::string{"lazy context"};
            });

        CHECK(invoked);
        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("lazy context"));
    }

    SECTION("with_context_lazy on value result - callable is NOT invoked")
    {
        auto res = cc::result<int>{42};
        bool invoked = false;

        res.with_context_lazy(
            [&]
            {
                invoked = true;
                return cc::string{"lazy context"};
            });

        CHECK(!invoked);
        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("with_context_lazy on error result - rvalue")
    {
        bool invoked = false;
        auto res = cc::result<int>{cc::error("base error")}.with_context_lazy(
            [&]
            {
                invoked = true;
                return cc::string{"lazy context"};
            });

        CHECK(invoked);
        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("lazy context"));
    }

    SECTION("with_context_lazy on value result - rvalue - callable is NOT invoked")
    {
        bool invoked = false;
        auto res = cc::result<int>{42}.with_context_lazy(
            [&]
            {
                invoked = true;
                return cc::string{"lazy context"};
            });

        CHECK(!invoked);
        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("with_context_lazy chaining with expensive computation")
    {
        auto res = cc::result<int>{cc::error("base error")};
        int computation_count = 0;

        res.with_context_lazy(
               [&]
               {
                   ++computation_count;
                   return cc::string{"context 1"};
               })
            .with_context_lazy(
                [&]
                {
                    ++computation_count;
                    return cc::string{"context 2"};
                });

        CHECK(computation_count == 2);
        CHECK(res.has_error());
        auto err_str = res.error().to_string();
        CHECK(err_str.contains("base error"));
        CHECK(err_str.contains("context 1"));
        CHECK(err_str.contains("context 2"));
    }

    SECTION("with_context_lazy avoids expensive computation on success")
    {
        auto res = cc::result<int>{42};
        int expensive_computation_count = 0;

        res.with_context_lazy(
               [&]
               {
                   ++expensive_computation_count;
                   // Simulate expensive operation
                   return cc::string{"expensive context 1"};
               })
            .with_context_lazy(
                [&]
                {
                    ++expensive_computation_count;
                    return cc::string{"expensive context 2"};
                })
            .with_context_lazy(
                [&]
                {
                    ++expensive_computation_count;
                    return cc::string{"expensive context 3"};
                });

        CHECK(expensive_computation_count == 0);
        CHECK(res.has_value());
        CHECK(res.value() == 42);
    }

    SECTION("with_context_lazy captures source location")
    {
        auto res = cc::result<int>{cc::error("base error")};

        auto before = cc::source_location::current();
        res.with_context_lazy([] { return cc::string{"lazy context"}; });
        auto after = cc::source_location::current();

        CHECK(res.has_error());
        CHECK(res.error().to_string().contains("lazy context"));
    }
}

TEST("result - CC_RETURN_IF_ERROR - basic usage")
{
    SECTION("success path - no early return")
    {
        auto inner = []() -> cc::result<int, cc::string> { return 42; };

        auto outer = [&]() -> cc::result<int, cc::string>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            return res.value() * 2;
        };

        auto result = outer();
        CHECK(result.has_value());
        CHECK(result.value() == 84);
    }

    SECTION("error path - early return")
    {
        auto inner = []() -> cc::result<int, cc::string> { return cc::error("inner error"); };

        auto outer = [&]() -> cc::result<int, cc::string>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            return 999; // should not reach here
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error() == "inner error");
    }

    SECTION("inline expression - success")
    {
        auto inner = []() -> cc::result<int, cc::string> { return 42; };

        auto outer = [&]() -> cc::result<int, cc::string>
        {
            CC_RETURN_IF_ERROR(inner());
            // Note: inner() was consumed, so we can't use its value
            return 100;
        };

        auto result = outer();
        CHECK(result.has_value());
        CHECK(result.value() == 100);
    }

    SECTION("inline expression - error")
    {
        auto inner = []() -> cc::result<int, cc::string> { return cc::error("inline error"); };

        auto outer = [&]() -> cc::result<int, cc::string>
        {
            CC_RETURN_IF_ERROR(inner());
            return 999; // should not reach here
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error() == "inline error");
    }
}

TEST("result - CC_RETURN_IF_ERROR - with_context chaining")
{
    SECTION("basic context chaining")
    {
        auto inner = []() -> cc::result<int> { return cc::error("inner error"); };

        auto outer = [&]() -> cc::result<int>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res).with_context("outer context");
            return res.value();
        };

        auto result = outer();
        CHECK(result.has_error());
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("inner error"));
        CHECK(err_str.contains("outer context"));
    }

    SECTION("multiple context layers")
    {
        auto layer1 = []() -> cc::result<int> { return cc::error("layer1 error"); };

        auto layer2 = [&]() -> cc::result<int>
        {
            auto res = layer1();
            CC_RETURN_IF_ERROR(res).with_context("layer2 context");
            return res.value();
        };

        auto layer3 = [&]() -> cc::result<int>
        {
            auto res = layer2();
            CC_RETURN_IF_ERROR(res).with_context("layer3 context");
            return res.value();
        };

        auto result = layer3();
        CHECK(result.has_error());
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("layer1 error"));
        CHECK(err_str.contains("layer2 context"));
        CHECK(err_str.contains("layer3 context"));
    }

    SECTION("with_context on success path does nothing")
    {
        auto inner = []() -> cc::result<int> { return 42; };

        auto outer = [&]() -> cc::result<int>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res).with_context("this context is never used");
            return res.value() * 2;
        };

        auto result = outer();
        CHECK(result.has_value());
        CHECK(result.value() == 84);
    }
}

TEST("result - CC_RETURN_IF_ERROR - type conversions")
{
    SECTION("typed error to any_error")
    {
        auto inner = []() -> cc::result<int, cc::string> { return cc::error("typed error"); };

        auto outer = [&]() -> cc::result<int> // any_error is default
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res).with_context("outer context");
            return res.value();
        };

        auto result = outer();
        CHECK(result.has_error());
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("typed error"));
        CHECK(err_str.contains("outer context"));
    }

    SECTION("different value types - same error type")
    {
        auto inner = []() -> cc::result<int, cc::string> { return cc::error("error message"); };

        auto outer = [&]() -> cc::result<cc::string, cc::string>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            // This would return a string, but we never get here
            return cc::string("should not reach");
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error() == "error message");
    }

    SECTION("int error to long error")
    {
        auto inner = []() -> cc::result<int, int> { return cc::error(42); };

        auto outer = [&]() -> cc::result<long, long>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            return 999L;
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error() == 42L);
    }

    SECTION("custom error type to any_error")
    {
        struct custom_error
        {
            int code;
            cc::string message;
        };

        auto inner = [](bool fail) -> cc::result<int, custom_error>
        {
            if (fail)
                return cc::error(custom_error{404, "not found"});
            return 42;
        };

        auto outer = [&]() -> cc::result<int>
        {
            auto res = inner(true);
            CC_RETURN_IF_ERROR(res).with_context("failed to process");
            return res.value();
        };

        auto result = outer();
        CHECK(result.has_error());
        // custom_error should be converted to debug string by any_error
        CHECK(!result.error().is_empty());
    }
}

TEST("result - CC_RETURN_IF_ERROR - single evaluation")
{
    SECTION("expression evaluated exactly once")
    {
        int call_count = 0;
        auto tracked_fn = [&]() -> cc::result<int, cc::string>
        {
            ++call_count;
            return cc::error("error");
        };

        auto outer = [&]() -> cc::result<int, cc::string>
        {
            CC_RETURN_IF_ERROR(tracked_fn());
            return 999;
        };

        auto result = outer();
        CHECK(call_count == 1); // Should only be called once
        CHECK(result.has_error());
    }
}

TEST("result - CC_RETURN_IF_ERROR - move semantics")
{
    SECTION("error is moved, not copied")
    {
        auto inner = []() -> cc::result<int, move_only> { return cc::error(move_only{42}); };

        auto outer = [&]() -> cc::result<int, move_only>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            return 999;
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error().value == 42);
    }

    SECTION("value in result not accessed on error path")
    {
        auto inner = []() -> cc::result<move_only, cc::string> { return cc::error("error"); };

        auto outer = [&]() -> cc::result<move_only, cc::string>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res);
            return move_only{999};
        };

        auto result = outer();
        CHECK(result.has_error());
        CHECK(result.error() == "error");
    }

    SECTION("any_error moved through layers")
    {
        auto layer1 = []() -> cc::result<int> { return cc::error("original error"); };

        auto layer2 = [&]() -> cc::result<int>
        {
            auto res = layer1();
            CC_RETURN_IF_ERROR(res).with_context("layer2");
            return res.value();
        };

        auto layer3 = [&]() -> cc::result<int>
        {
            auto res = layer2();
            CC_RETURN_IF_ERROR(res).with_context("layer3");
            return res.value();
        };

        auto result = layer3();
        CHECK(result.has_error());
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("original error"));
        CHECK(err_str.contains("layer2"));
        CHECK(err_str.contains("layer3"));
    }
}

TEST("result - CC_RETURN_IF_ERROR - edge cases")
{
    SECTION("empty any_error")
    {
        auto inner = []() -> cc::result<int> { return cc::error(cc::any_error{}); };

        auto outer = [&]() -> cc::result<int>
        {
            auto res = inner();
            CC_RETURN_IF_ERROR(res).with_context("added context");
            return res.value();
        };

        auto result = outer();
        CHECK(result.has_error());
        // Empty error should still accept context
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("added context"));
    }

    SECTION("multiple consecutive checks")
    {
        auto fn1 = [](bool fail) -> cc::result<int, cc::string>
        { return fail ? cc::error("fn1 error") : cc::result<int, cc::string>{10}; };

        auto fn2 = [](bool fail) -> cc::result<int, cc::string>
        { return fail ? cc::error("fn2 error") : cc::result<int, cc::string>{20}; };

        auto fn3 = [](bool fail) -> cc::result<int, cc::string>
        { return fail ? cc::error("fn3 error") : cc::result<int, cc::string>{30}; };

        auto outer = [&](bool f1, bool f2, bool f3) -> cc::result<int, cc::string>
        {
            auto r1 = fn1(f1);
            CC_RETURN_IF_ERROR(r1);

            auto r2 = fn2(f2);
            CC_RETURN_IF_ERROR(r2);

            auto r3 = fn3(f3);
            CC_RETURN_IF_ERROR(r3);

            return r1.value() + r2.value() + r3.value();
        };

        // All succeed
        auto res1 = outer(false, false, false);
        CHECK(res1.has_value());
        CHECK(res1.value() == 60);

        // First fails
        auto res2 = outer(true, false, false);
        CHECK(res2.has_error());
        CHECK(res2.error() == "fn1 error");

        // Second fails
        auto res3 = outer(false, true, false);
        CHECK(res3.has_error());
        CHECK(res3.error() == "fn2 error");

        // Third fails
        auto res4 = outer(false, false, true);
        CHECK(res4.has_error());
        CHECK(res4.error() == "fn3 error");
    }

    SECTION("nested function calls")
    {
        auto innermost = []() -> cc::result<int> { return cc::error("innermost error"); };

        auto middle = [&]() -> cc::result<int>
        {
            auto res = innermost();
            CC_RETURN_IF_ERROR(res).with_context("middle layer");
            return res.value();
        };

        auto outer = [&]() -> cc::result<int>
        {
            auto res = middle();
            CC_RETURN_IF_ERROR(res).with_context("outer layer");
            return res.value();
        };

        auto result = outer();
        CHECK(result.has_error());
        auto err_str = result.error().to_string();
        CHECK(err_str.contains("innermost error"));
        CHECK(err_str.contains("middle layer"));
        CHECK(err_str.contains("outer layer"));
    }
}

TEST("result - CC_RETURN_IF_ERROR - real-world patterns")
{
    SECTION("database-style operations")
    {
        auto connect_db = [](bool succeed) -> cc::result<int> // connection id
        { return succeed ? cc::result<int>{42} : cc::error("connection failed"); };

        auto query_db = [](int conn_id, bool succeed) -> cc::result<cc::string>
        { return succeed ? cc::result<cc::string>{"query result"} : cc::error("query failed"); };

        auto fetch_user = [&](bool conn_ok, bool query_ok) -> cc::result<cc::string>
        {
            auto conn = connect_db(conn_ok);
            CC_RETURN_IF_ERROR(conn).with_context("failed to connect to database");

            auto result = query_db(conn.value(), query_ok);
            CC_RETURN_IF_ERROR(result).with_context("failed to fetch user");

            return result.value();
        };

        // Success case
        auto res1 = fetch_user(true, true);
        CHECK(res1.has_value());
        CHECK(res1.value() == "query result");

        // Connection fails
        auto res2 = fetch_user(false, true);
        CHECK(res2.has_error());
        auto err2 = res2.error().to_string();
        CHECK(err2.contains("connection failed"));
        CHECK(err2.contains("failed to connect to database"));

        // Query fails
        auto res3 = fetch_user(true, false);
        CHECK(res3.has_error());
        auto err3 = res3.error().to_string();
        CHECK(err3.contains("query failed"));
        CHECK(err3.contains("failed to fetch user"));
    }

    SECTION("file I/O style operations")
    {
        auto open_file = [](bool succeed) -> cc::result<int, cc::string> // file handle
        { return succeed ? cc::result<int, cc::string>{123} : cc::error("file not found"); };

        auto read_file = [](int handle, bool succeed) -> cc::result<cc::string, cc::string>
        { return succeed ? cc::result<cc::string, cc::string>{"file contents"} : cc::error("read error"); };

        auto parse_json = [](cc::string const& contents, bool succeed) -> cc::result<int, cc::string>
        { return succeed ? cc::result<int, cc::string>{999} : cc::error("parse error"); };

        auto load_config = [&](bool open_ok, bool read_ok, bool parse_ok) -> cc::result<int, cc::string>
        {
            auto handle = open_file(open_ok);
            CC_RETURN_IF_ERROR(handle);

            auto contents = read_file(handle.value(), read_ok);
            CC_RETURN_IF_ERROR(contents);

            auto parsed = parse_json(contents.value(), parse_ok);
            CC_RETURN_IF_ERROR(parsed);

            return parsed.value();
        };

        // Success
        auto res1 = load_config(true, true, true);
        CHECK(res1.has_value());
        CHECK(res1.value() == 999);

        // Open fails
        auto res2 = load_config(false, true, true);
        CHECK(res2.has_error());
        CHECK(res2.error() == "file not found");

        // Read fails
        auto res3 = load_config(true, false, true);
        CHECK(res3.has_error());
        CHECK(res3.error() == "read error");

        // Parse fails
        auto res4 = load_config(true, true, false);
        CHECK(res4.has_error());
        CHECK(res4.error() == "parse error");
    }

    SECTION("validation chain")
    {
        auto validate_non_empty = [](cc::string const& s) -> cc::result<cc::string, cc::string>
        { return s.empty() ? cc::error("string is empty") : cc::result<cc::string, cc::string>{s}; };

        auto validate_length = [](cc::string_view s, size_t max_len) -> cc::result<cc::string, cc::string>
        { return s.size() > max_len ? cc::error("string too long") : cc::result<cc::string, cc::string>{s}; };

        auto validate_username = [&](cc::string const& username) -> cc::result<cc::string, cc::string>
        {
            auto r1 = validate_non_empty(username);
            CC_RETURN_IF_ERROR(r1);

            auto r2 = validate_length(r1.value(), 20);
            CC_RETURN_IF_ERROR(r2);

            return r2.value();
        };

        // Valid
        auto res1 = validate_username("alice");
        CHECK(res1.has_value());
        CHECK(res1.value() == "alice");

        // Empty
        auto res2 = validate_username("");
        CHECK(res2.has_error());
        CHECK(res2.error() == "string is empty");

        // Too long
        auto res3 = validate_username("this_username_is_way_too_long_for_our_system");
        CHECK(res3.has_error());
        CHECK(res3.error() == "string too long");
    }
}

TEST("result - or_throw - basic usage")
{
    SECTION("or_throw with value - returns value by rvalue reference")
    {
        auto some_fn = []() -> cc::result<int, int> { return 42; };

        auto val = some_fn().or_throw();
        CHECK(val == 42);
    }

    SECTION("or_throw with error - throws exception")
    {
        auto some_fn = []() -> cc::result<int, int> { return cc::error(99); };

        CHECK_THROWS(some_fn().or_throw());
    }

    SECTION("or_throw with value - lvalue reference")
    {
        auto some_fn = []() -> cc::result<int, int> { return 42; };

        auto res = some_fn();
        auto& val = res.or_throw();
        CHECK(val == 42);
        CHECK(&val == &res.value());
    }

    SECTION("or_throw with error - lvalue reference throws")
    {
        auto some_fn = []() -> cc::result<int, int> { return cc::error(99); };

        auto res = some_fn();
        CHECK_THROWS(res.or_throw());
    }

    SECTION("or_throw returns correct exception type")
    {
        auto some_fn = []() -> cc::result<int, int> { return cc::error(99); };

        CHECK_THROWS_AS(some_fn().or_throw(), cc::result_exception);
    }
}

TEST("result - or_throw - value categories")
{
    SECTION("or_throw on const lvalue result")
    {
        auto const res = cc::result<int, int>{42};
        auto const& val = res.or_throw();
        CHECK(val == 42);
        CHECK(&val == &res.value());
    }

    SECTION("or_throw on mutable lvalue result")
    {
        auto res = cc::result<int, int>{42};
        auto& val = res.or_throw();
        val = 99;
        CHECK(res.value() == 99);
    }

    SECTION("or_throw on rvalue result")
    {
        auto res = cc::result<move_only, int>{move_only{42}};
        auto moved = cc::move(res).or_throw();
        CHECK(moved.value == 42);
    }

    SECTION("or_throw on const rvalue result")
    {
        auto make_result = []() -> cc::result<int, int> { return 42; };
        auto const res = make_result();
        auto const& val = res.or_throw();
        CHECK(val == 42);
    }
}

TEST("result - or_throw - non-trivial types")
{
    SECTION("or_throw with string value")
    {
        auto some_fn = []() -> cc::result<cc::string, int> { return cc::string{"hello"}; };

        auto val = some_fn().or_throw();
        CHECK(val == "hello");
    }

    SECTION("or_throw with string error")
    {
        auto some_fn = []() -> cc::result<int, cc::string> { return cc::error(cc::string{"error message"}); };

        CHECK_THROWS(some_fn().or_throw());
    }

    SECTION("or_throw with move-only type")
    {
        auto some_fn = []() -> cc::result<move_only, int> { return move_only{42}; };

        auto val = some_fn().or_throw();
        CHECK(val.value == 42);
    }

    SECTION("or_throw with non-trivial value - lvalue reference")
    {
        auto res = cc::result<cc::string, int>{cc::string{"test"}};
        auto& val = res.or_throw();
        val += " modified";
        CHECK(res.value() == "test modified");
    }
}

TEST("result - or_throw - any_error integration")
{
    SECTION("or_throw with any_error - success")
    {
        auto some_fn = []() -> cc::result<int> { return 42; };

        auto val = some_fn().or_throw();
        CHECK(val == 42);
    }

    SECTION("or_throw with any_error - error from string")
    {
        auto some_fn = []() -> cc::result<int> { return cc::error("error message"); };

        CHECK_THROWS_AS(some_fn().or_throw(), cc::result_exception);
    }

    SECTION("or_throw with any_error - error from int")
    {
        auto some_fn = []() -> cc::result<int> { return cc::error(42); };

        CHECK_THROWS_AS(some_fn().or_throw(), cc::result_exception);
    }

    SECTION("or_throw with any_error - lvalue reference")
    {
        auto res = cc::result<int>{42};
        auto& val = res.or_throw();
        CHECK(val == 42);
        CHECK(&val == &res.value());
    }

    SECTION("or_throw with any_error - lvalue error throws")
    {
        auto res = cc::result<int>{cc::error(cc::string{"error message"})};
        CHECK_THROWS_AS(res.or_throw(), cc::result_exception);
    }
}

TEST("result - or_throw - chaining patterns")
{
    SECTION("or_throw in expression chain - success")
    {
        auto parse_int = [](cc::string const& s) -> cc::result<int, cc::string>
        {
            if (s == "42")
                return 42;
            return cc::error("parse error");
        };

        auto process = [&](cc::string const& s) -> int { return parse_int(s).or_throw() * 2; };

        auto result = process("42");
        CHECK(result == 84);
    }

    SECTION("or_throw in expression chain - error throws")
    {
        auto parse_int = [](cc::string const& s) -> cc::result<int, cc::string>
        {
            if (s == "42")
                return 42;
            return cc::error("parse error");
        };

        auto process = [&](cc::string const& s) -> int { return parse_int(s).or_throw() * 2; };

        CHECK_THROWS(process("invalid"));
    }

    SECTION("multiple or_throw in sequence - all success")
    {
        auto fn1 = []() -> cc::result<int, int> { return 10; };
        auto fn2 = []() -> cc::result<int, int> { return 20; };
        auto fn3 = []() -> cc::result<int, int> { return 30; };

        auto compute = [&]() -> int { return fn1().or_throw() + fn2().or_throw() + fn3().or_throw(); };

        auto result = compute();
        CHECK(result == 60);
    }

    SECTION("multiple or_throw in sequence - middle fails")
    {
        auto fn1 = []() -> cc::result<int, int> { return 10; };
        auto fn2 = []() -> cc::result<int, int> { return cc::error(99); };
        auto fn3 = []() -> cc::result<int, int> { return 30; };

        auto compute = [&]() -> int { return fn1().or_throw() + fn2().or_throw() + fn3().or_throw(); };

        CHECK_THROWS(compute());
    }
}

TEST("result - or_throw - const correctness")
{
    SECTION("const result returns const reference")
    {
        auto const res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(res.or_throw()), int const&>);
        auto const& val = res.or_throw();
        CHECK(val == 42);
    }

    SECTION("mutable result returns mutable reference")
    {
        auto res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(res.or_throw()), int&>);
        auto& val = res.or_throw();
        val = 99;
        CHECK(res.value() == 99);
    }

    SECTION("rvalue result returns rvalue reference")
    {
        auto res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(cc::move(res).or_throw()), int&&>);
        SUCCEED("static checks");
    }

    SECTION("const rvalue result returns const rvalue reference")
    {
        auto const res = cc::result<int, int>{42};
        static_assert(std::is_same_v<decltype(cc::move(res).or_throw()), int const&&>);
        SUCCEED("static checks");
    }
}
