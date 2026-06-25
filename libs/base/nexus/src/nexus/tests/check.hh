#pragma once

#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/string/to_debug_string.hh>

#include <type_traits>

namespace nx::impl
{
// Check kind: soft (CHECK) vs require (REQUIRE)
enum class check_kind
{
    check,
    require
};

// Comparison operators
enum class cmp_op
{
    none,
    less,
    less_equal,
    greater,
    greater_equal,
    equal,
    not_equal,
    throws,
    throws_as,
    assert_fail, // "unexpected assertion"
    asserts,     // "expected assertion"
    skip,        // "test skipped"
};

// Exception type thrown by our custom assertion handler to signal expected assertion failure
struct expected_assertion_exception
{
};

// Sink that collects check outcomes instead of recording them on the active test.
// Used by tools (e.g. the fuzz engine) that drive user code which is expected to fail often:
// while a sink is installed, CHECK/REQUIRE failures are tallied here, no test error is recorded,
// and REQUIRE/SKIP no longer throw to abort. The first failure message is kept for diagnostics.
struct check_capture_sink
{
    int executed = 0;
    int failed = 0;
    bool require_failed = false; // a REQUIRE (or assertion) failed -> the run must stop
    cc::string first_message;    // expanded message of the first failure, if any
};

// RAII installer for a capture sink (thread-local, one active at a time).
struct scoped_check_capture
{
    explicit scoped_check_capture(check_capture_sink& sink);
    ~scoped_check_capture();

    scoped_check_capture(scoped_check_capture const&) = delete;
    scoped_check_capture& operator=(scoped_check_capture const&) = delete;
    scoped_check_capture(scoped_check_capture&&) = delete;
    scoped_check_capture& operator=(scoped_check_capture&&) = delete;
};

// Forward declarations
struct check_handle;
template <class L>
struct lhs_holder;
template <class L, class R>
struct binary_expr_capture;

// Tag for expression decomposition
struct lhs_grab
{
    // Overload operator<=> to capture lhs, hidden friend for less namespace pollution
    template <class L>
    friend auto operator<=>(lhs_grab, L const& lhs)
    {
        return lhs_holder<L>{lhs};
    }
};

// Intermediate expression holder that captures lhs and waits for comparison
template <class L>
struct lhs_holder
{
    // note: this only lives during the eval of CHECK/REQUIRE and is thus safe
    L const& lhs;

    explicit lhs_holder(L const& lhs) : lhs(lhs) {}

    // Comparison operators that build binary_expr_capture
    template <class R>
    auto operator<(R const& rhs) const
    {
        static_assert(requires { bool(lhs < rhs); }, "lhs < rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::less, bool(lhs < rhs)};
    }

    template <class R>
    auto operator<=(R const& rhs) const
    {
        static_assert(requires { bool(lhs <= rhs); }, "lhs <= rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::less_equal, bool(lhs <= rhs)};
    }

    template <class R>
    auto operator>(R const& rhs) const
    {
        static_assert(requires { bool(lhs > rhs); }, "lhs > rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::greater, bool(lhs > rhs)};
    }

    template <class R>
    auto operator>=(R const& rhs) const
    {
        static_assert(requires { bool(lhs >= rhs); }, "lhs >= rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::greater_equal, bool(lhs >= rhs)};
    }

    template <class R>
    auto operator==(R const& rhs) const
    {
        static_assert(requires { bool(lhs == rhs); }, "lhs == rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::equal, bool(lhs == rhs)};
    }

    template <class R>
    auto operator!=(R const& rhs) const
    {
        static_assert(requires { bool(lhs != rhs); }, "lhs != rhs must be a valid expression");
        return binary_expr_capture<L, R>{lhs, rhs, cmp_op::not_equal, bool(lhs != rhs)};
    }

    // TODO: && and || guardrails
};

// Binary expression capture: stores lhs, rhs, operator, and passed
template <class L, class R>
struct binary_expr_capture
{
    // note: this only lives during the eval of CHECK/REQUIRE and is thus safe
    L const& lhs;
    R const& rhs;
    cmp_op op;
    bool passed;
};

// Check handle for chaining and deferred failure reporting
struct check_handle final
{
    struct impl_context;
    cc::unique_ptr<impl_context> ctx;
    bool passed = false;

    check_handle() = default;
    check_handle(check_handle&&) = default;
    check_handle(check_handle const&) = delete;
    check_handle& operator=(check_handle&&) = default;
    check_handle& operator=(check_handle const&) = delete;

    ~check_handle() noexcept(false);

    check_handle context(cc::string msg) &&;
    check_handle note(cc::string msg) &&;

    check_handle fail_note() &&;
    check_handle fail_note(cc::string msg) &&;
    check_handle succeed_note() &&;
    check_handle succeed_note(cc::string msg) &&;

    template <class T>
    check_handle dump(cc::string_view label, T const& value) &&
    {
        return cc::move(*this).add_extra_line(passed ? cc::string()
                                                     : cc::format("{}: {}", label, cc::to_debug_string(value)));
    }

    // 2 dumps is used for CHECK(lhs op rhs)
    // 1 dump for CHECK(value)
    template <class T>
    check_handle dump(T const& value) &&
    {
        return cc::move(*this).add_extra_line(passed ? cc::string() : cc::to_debug_string(value));
    }

    static check_handle make(check_kind kind, cmp_op op, char const* expr_text, bool passed, cc::source_location loc);

private:
    check_handle add_extra_line(cc::string line) &&;
};

// Factory function for check_handle
template <class L, class R>
check_handle make_check_handle(check_kind kind,
                               char const* expr_text,
                               binary_expr_capture<L, R> const& expr,
                               cc::source_location loc)
{
    return check_handle::make(kind, expr.op, expr_text, expr.passed, loc) //
        .dump(expr.lhs)                                                   //
        .dump(expr.rhs);
}

template <class T>
check_handle make_check_handle(check_kind kind, char const* expr_text, lhs_holder<T> const& expr, cc::source_location loc)
{
    static_assert(requires(T const& v) { bool(v); }, "type must be castable to bool in CHECK/REQUIRE(v)");

    return check_handle::make(kind, cmp_op::none, expr_text, bool(expr.lhs), loc);
}

// Helper for exception checking (any exception)
template <class F>
check_handle make_check_handle_throws(check_kind kind, char const* expr_text, F&& func, cc::source_location loc)
{
    bool threw_exception = false;
    cc::string exception_info;

    try
    {
        func();
        exception_info = "no exception was thrown (but should have been)";
    }
    catch (...)
    {
        threw_exception = true;
    }

    return check_handle::make(kind, cmp_op::throws, expr_text, threw_exception, loc).context(cc::move(exception_info));
}

// Helper for exception type checking
template <class ExceptionType, class F>
check_handle make_check_handle_throws_as(check_kind kind,
                                         char const* expr_text, // NOLINT
                                         char const* exception_type_text,
                                         F&& func,
                                         cc::source_location loc)
{
    bool threw_correct_type = false;
    cc::string exception_info;

    try
    {
        func();
        exception_info = "no exception was thrown (but should have been)";
    }
    catch (ExceptionType const& e)
    {
        threw_correct_type = true;
    }
    catch (...)
    {
        exception_info = "caught wrong exception type, expected: ";
        exception_info += exception_type_text;
    }

    return check_handle::make(kind, cmp_op::throws_as, expr_text, threw_correct_type, loc).context(cc::move(exception_info));
}

// Helper for assertion failure checking
// This sets up a scoped assertion handler that throws expected_assertion_exception
// If an assertion fires, we catch it and report success
// If no assertion fires (or if assertions are disabled), we report failure (unless assertions are disabled)
template <class F>
check_handle make_check_handle_asserts(check_kind kind, char const* expr_text, F&& func, cc::source_location loc)
{
#if CC_ASSERT_ENABLED
    bool assertion_fired = false;
    cc::string assertion_info;

    {
        // Set up a scoped assertion handler that throws on assertion failure
        auto handler = cc::impl::scoped_assertion_handler([](cc::impl::assertion_info const&)
                                                          { throw expected_assertion_exception{}; });

        try
        {
            func();
            assertion_info = "no assertion was triggered (but should have been)";
        }
        catch (expected_assertion_exception const&)
        {
            assertion_fired = true;
        }
    }

    return check_handle::make(kind, cmp_op::asserts, expr_text, assertion_fired, loc).context(cc::move(assertion_info));
#else
    // Assertions are disabled - report success without executing the function
    // (executing the function could lead to UB since assertions are compiled out)
    return check_handle::make(kind, cmp_op::asserts, expr_text, true, loc);
#endif
}

} // namespace nx::impl

// CHECK macro: soft assertion that continues test execution on failure
// Supports boolean expressions and comparisons, preserving lhs/rhs values for diagnostics
// Returns check_handle that can be chained with .note(), .context(), .dump(), etc.
//
// Examples:
//   CHECK(ptr != nullptr);
//   CHECK(value == 42);
//   CHECK(a < b).context("comparing sizes");
//   CHECK(result).note("expected truthy result").dump("result", result);
#define CHECK(Expr)                                                                                      \
    ::nx::impl::make_check_handle(::nx::impl::check_kind::check, #Expr, ::nx::impl::lhs_grab{} <=> Expr, \
                                  cc::source_location::current())

// REQUIRE macro: hard assertion that stops test execution on failure
// Supports boolean expressions and comparisons, preserving lhs/rhs values for diagnostics
// Returns check_handle that can be chained with .note(), .context(), .dump(), etc.
//
// Examples:
//   REQUIRE(file.is_open());
//   REQUIRE(count > 0);
//   REQUIRE(ptr != nullptr).context("initialization phase");
#define REQUIRE(Expr)                                                                                      \
    ::nx::impl::make_check_handle(::nx::impl::check_kind::require, #Expr, ::nx::impl::lhs_grab{} <=> Expr, \
                                  cc::source_location::current())

// FAIL macro: unconditional hard failure (like REQUIRE(false))
// Optionally takes a message argument
// Returns check_handle that can be chained with other helper functions
//
// Examples:
//   FAIL();                              // fails with default note
//   FAIL("unexpected code path");        // fails with custom message
//   FAIL("invalid state").context("cleanup phase");
#define FAIL(...)                                                                                            \
    ::nx::impl::check_handle::make(::nx::impl::check_kind::require, ::nx::impl::cmp_op::none, "FAIL", false, \
                                   cc::source_location::current())                                           \
        .fail_note(__VA_ARGS__)

// SUCCEED macro: unconditional soft success (like CHECK(true))
// Optionally takes a message argument
// Returns check_handle that can be chained with other helper functions
//
// Examples:
//   SUCCEED();                           // succeeds with default note
//   SUCCEED("reached milestone");        // succeeds with custom message
//   SUCCEED("validation passed").dump("data", validated_data);
#define SUCCEED(...)                                                                                         \
    ::nx::impl::check_handle::make(::nx::impl::check_kind::check, ::nx::impl::cmp_op::none, "SUCCEED", true, \
                                   cc::source_location::current())                                           \
        .succeed_note(__VA_ARGS__)

// SKIP macro: skips the current test (like SUCCEED but aborts execution)
// Optionally takes a message argument
// Returns check_handle that can be chained with other helper functions
//
// Examples:
//   SKIP();                              // skips with default note
//   SKIP("not implemented yet");         // skips with custom message
//
// TODO: currently doesn't interact with SECTION properly
#define SKIP(...)                                                                                         \
    ::nx::impl::check_handle::make(::nx::impl::check_kind::check, ::nx::impl::cmp_op::skip, "SKIP", true, \
                                   cc::source_location::current())                                        \
        .succeed_note(__VA_ARGS__)

// CHECK_THROWS macro: soft assertion that expression throws any exception
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Examples:
//   CHECK_THROWS(throw_function());
//   CHECK_THROWS(risky_operation()).context("testing error handling");
#define CHECK_THROWS(Expr)                \
    ::nx::impl::make_check_handle_throws( \
        ::nx::impl::check_kind::check, #Expr, [&] { (void)(Expr); }, cc::source_location::current())

// CHECK_THROWS_AS macro: soft assertion that expression throws specific exception type
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Examples:
//   CHECK_THROWS_AS(divide_by_zero(), std::runtime_error);
//   CHECK_THROWS_AS(parse("bad"), ParseException).context("validation");
#define CHECK_THROWS_AS(Expr, ExceptionType)                \
    ::nx::impl::make_check_handle_throws_as<ExceptionType>( \
        ::nx::impl::check_kind::check, #Expr, #ExceptionType, [&] { (void)(Expr); }, cc::source_location::current())

// REQUIRE_THROWS macro: hard assertion that expression throws any exception
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Examples:
//   REQUIRE_THROWS(throw_function());
//   REQUIRE_THROWS(must_fail()).context("precondition check");
#define REQUIRE_THROWS(Expr)              \
    ::nx::impl::make_check_handle_throws( \
        ::nx::impl::check_kind::require, #Expr, [&] { (void)(Expr); }, cc::source_location::current())

// REQUIRE_THROWS_AS macro: hard assertion that expression throws specific exception type
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Examples:
//   REQUIRE_THROWS_AS(invalid_operation(), std::logic_error);
//   REQUIRE_THROWS_AS(connect("bad"), ConnectionException).note("setup phase");
#define REQUIRE_THROWS_AS(Expr, ExceptionType)              \
    ::nx::impl::make_check_handle_throws_as<ExceptionType>( \
        ::nx::impl::check_kind::require, #Expr, #ExceptionType, [&] { (void)(Expr); }, cc::source_location::current())

// CHECK_ASSERTS macro: soft assertion that expression triggers a CC_ASSERT
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Captures assertion failures via scoped assertion handler that throws expected_assertion_exception.
// When assertions are disabled (CC_ASSERT_ENABLED == 0), reports success without executing the expression.
//
// Examples:
//   CHECK_ASSERTS(divide_by_zero(0));
//   CHECK_ASSERTS(invalid_operation()).context("testing precondition");
#define CHECK_ASSERTS(Expr)                \
    ::nx::impl::make_check_handle_asserts( \
        ::nx::impl::check_kind::check, #Expr, [&] { (void)(Expr); }, cc::source_location::current())

// REQUIRE_ASSERTS macro: hard assertion that expression triggers a CC_ASSERT
// Returns check_handle that can be chained with .note(), .context(), etc.
//
// Captures assertion failures via scoped assertion handler that throws expected_assertion_exception.
// When assertions are disabled (CC_ASSERT_ENABLED == 0), reports success without executing the expression.
//
// Examples:
//   REQUIRE_ASSERTS(must_assert());
//   REQUIRE_ASSERTS(out_of_bounds_access()).note("boundary check");
#define REQUIRE_ASSERTS(Expr)              \
    ::nx::impl::make_check_handle_asserts( \
        ::nx::impl::check_kind::require, #Expr, [&] { (void)(Expr); }, cc::source_location::current())
