#pragma once

#include <clean-core/asserts.hh>
#include <clean-core/fwd.hh>
#include <clean-core/node_allocation.hh>
#include <clean-core/source_location.hh>
#include <clean-core/stacktrace.hh>
#include <clean-core/string.hh>
#include <clean-core/string_view.hh>
#include <clean-core/to_debug_string.hh>
#include <clean-core/utility.hh>

#include <type_traits>

// TODO:
// - maybe plan a custom site-propagation protocol, so error type conversions can track original sites

// =========================================================================================================
// cc::any_error - Move-only error container with context chaining
// =========================================================================================================

/// Move-only error type optimized for debugging and context accumulation.
/// Captures source location by default, optionally captures stacktrace.
/// Supports additive context chaining (à la anyhow) for rich error reporting.
/// Single move-only T* under the hood, so register-sized and minimal overhead.
struct cc::any_error
{
public:
    /// Default constructor creates an error with no message.
    /// Only available to support result<T, E> default construction when E is default-constructible.
    any_error() = default;

    /// Construct from a string message, capturing source location.
    explicit any_error(cc::string message, cc::source_location site = cc::source_location::current());

    /// Construct from another any_error (move), with explicit source location.
    /// This two-argument overload is used by CC_RETURN_IF_ERROR macro.
    /// The source location is ignored and the original error's site is preserved.
    explicit any_error(any_error&& e, cc::source_location site) noexcept : _payload(cc::move(e._payload))
    {
        (void)site; // ignored, keeps original error's site
    }

    /// Construct from any error type E, capturing source location.
    /// The error is converted to a debug string representation.
    template <class E>
        requires(!std::is_same_v<std::remove_cvref_t<E>, any_error> && !std::is_same_v<std::remove_cvref_t<E>, cc::string>)
    explicit any_error(E&& e, cc::source_location site = cc::source_location::current())
      : any_error(cc::default_node_allocator(), cc::to_debug_string(cc::forward<E>(e)), site)
    {
    }

    any_error(any_error const&) = delete;
    any_error(any_error&&) noexcept;
    any_error& operator=(any_error const&) = delete;
    any_error& operator=(any_error&&) noexcept;
    ~any_error();

    /// Add context to the error chain (lvalue version).
    /// Returns *this for chaining.
    /// NOTE: it's valid to call this on an empty cc::any_error
    any_error& with_context(cc::string message, cc::source_location site = cc::source_location::current()) &;

    /// Add context to the error chain (rvalue version).
    /// Returns the moved error for chaining.
    /// NOTE: it's valid to call this on an empty cc::any_error
    any_error with_context(cc::string message, cc::source_location site = cc::source_location::current()) &&;

    /// Get the source location where this error was created.
    /// NOTE: only reliable if not !is_empty()
    [[nodiscard]] cc::source_location site() const;

    /// Check if a stacktrace was captured.
    [[nodiscard]] bool has_stacktrace() const;

    /// Get the stacktrace if captured, nullptr otherwise.
    [[nodiscard]] cc::stacktrace const* get_stacktrace() const;

    /// Convert the error and all its context to a string for debugging.
    /// NOTE: only reliable if not !is_empty()
    [[nodiscard]] cc::string to_string() const;

    /// True if this is default constructed without a user-provided message.
    [[nodiscard]] bool is_empty() const;

private:
    explicit any_error(cc::node_allocator& alloc, cc::string message, cc::source_location site);

    // creates _payload if not already done
    // this is done to make sure an empty any_error does not default-alloc
    // but can have attached context later on
    void impl_ensure_payload();

    struct payload;
    struct context_node;

    cc::node_allocation<payload> _payload;
};

// =========================================================================================================
// cc::as_error_t - Explicit error wrapper for result construction
// =========================================================================================================

/// Explicit wrapper for error values in result construction.
/// Forces error construction to be explicit via cc::error(E).
/// This prevents implicit conversion from types that could be either T or E.
template <class E>
struct cc::as_error_t
{
public:
    explicit constexpr as_error_t(E const& e, cc::source_location s) : _e(e), _site(s) {}
    explicit constexpr as_error_t(E&& e, cc::source_location s) : _e(cc::move(e)), _site(s) {}

    /// Add context to the error (rvalue version).
    /// Converts to as_error_t<any_error> with the context attached.
    /// Only available for rvalue references (you should never store cc::error(...) anyway).
    /// This enables chaining like: CC_RETURN_IF_ERROR(res).with_context("context")
    [[nodiscard]] as_error_t<any_error> with_context(cc::string message,
                                                     cc::source_location context_site = cc::source_location::current()) &&
    {
        return as_error_t<any_error>(any_error(cc::move(_e), _site).with_context(cc::move(message), context_site),
                                     context_site);
    }

private:
    E _e;
    cc::source_location _site;

    template <class T, class F>
    friend struct result;
};

namespace cc
{
/// Factory function to construct an explicit error wrapper for use with cc::result.
///
/// This is the primary way to *return* an error from functions that use cc::result.
/// It marks the value as an error unambiguously and avoids accidental success/error
/// confusion in overload resolution and return type deduction.
///
/// Typical usage:
///   return cc::error("failure message");
///   return cc::error(my_typed_error{...});
///
/// The returned wrapper does not allocate by itself. It merely tags the value as
/// an error; the actual error object is constructed inside the receiving
/// cc::result<T, E>.
///
/// When used with the default error type (cc::any_error), this enables concise
/// application-level error handling while preserving rich debugging information
/// (source location, context chains, optional stack traces).
///
/// Error construction is intentionally explicit. This function is the single
/// sanctioned escape hatch from the success path.
///
/// The optional source location defaults to the call site and is propagated into
/// cc::any_error (or ignored by error types that do not store locations).
///
/// Usage: return cc::error("failure message");
template <class E>
[[nodiscard]] constexpr as_error_t<std::decay_t<E>> error(E&& e, cc::source_location s = cc::source_location::current())
{
    return as_error_t<std::decay_t<E>>(cc::forward<E>(e), s);
}
// NOTE: overload for string literals to prevent decay to char const*
//       (ensures to_debug_string sees string, not char const*)
template <size_t N>
[[nodiscard]] constexpr as_error_t<cc::string> error(char const (&str)[N],
                                                     cc::source_location s = cc::source_location::current())
{
    return as_error_t<cc::string>(cc::string(str, isize(N - 1)), s);
}
} // namespace cc

// =========================================================================================================
// cc::result_exception - Exception type for or_throw()
// =========================================================================================================

/// Exception type thrown by result::or_throw() when converting an error to an exception.
/// Stores the error as cc::any_error to support any error type E via type erasure.
struct cc::result_exception
{
public:
    /// Construct from any error type E via type erasure.
    template <class E>
    [[nodiscard]] static result_exception from_error(E&& e)
    {
        return result_exception(cc::any_error(cc::forward<E>(e)));
    }

    /// Get the underlying error.
    [[nodiscard]] cc::any_error const& error() const& { return _error; }
    [[nodiscard]] cc::any_error& error() & { return _error; }
    [[nodiscard]] cc::any_error&& error() && { return cc::move(_error); }

private:
    explicit result_exception(cc::any_error&& e) : _error(cc::move(e)) {}

    cc::any_error _error;
};

// =========================================================================================================
// cc::result<T, E> - Tagged union representing success or failure
// =========================================================================================================

/// Tagged union representing either a value of type T or an error of type E.
/// Success is the default semantic path; error construction is always explicit via cc::as_error(E).
/// The representation is a raw tagged union; no variant, no dynamic dispatch, no hidden allocations.
/// Trivial special members when T and E are trivial.
template <class T, class E>
struct [[nodiscard]] cc::result
{
    static_assert(!std::is_reference_v<T>, "result<T&, E> is not allowed - references are banned");
    static_assert(!std::is_reference_v<E>, "result<T, E&> is not allowed - references are banned");
    static_assert(!std::is_void_v<T>,
                  "result<void, E> is not allowed - use result<unit, E> instead (did you miss a cc::regular_invoke?)");
    static_assert(!std::is_void_v<E>, "result<T, void> is not allowed");

public:
    using value_type = T;
    using error_type = E;

    // construction
public:
    /// Default constructor creates an error state with E{}.
    /// Only enabled when E is default-constructible.
    constexpr result()
        requires(std::is_default_constructible_v<E>)
    {
        new (cc::placement_new, &_e) E();
    }

    /// Construct from a value (implicit when convertible).
    constexpr result(T const& v)
        requires(std::is_copy_constructible_v<T>)
      : _has_value(true)
    {
        new (cc::placement_new, &_v) T(v);
    }

    /// Construct from a value (rvalue, implicit when convertible).
    constexpr result(T&& v)
        requires(std::is_move_constructible_v<T>)
      : _has_value(true)
    {
        new (cc::placement_new, &_v) T(cc::move(v));
    }

    /// Construct from an explicit error wrapper (move).
    /// NOTE: rvalue reference only, because cc::error should not be stored
    template <class F>
    constexpr result(as_error_t<F>&& e)
        requires(std::is_constructible_v<E, F &&>)
    {
        if constexpr (std::is_constructible_v<E, F&&, cc::source_location>)
            new (cc::placement_new, &_e) E(cc::move(e._e), e._site);
        else
            new (cc::placement_new, &_e) E(cc::move(e._e));
    }

    /// Explicit converting constructor from result<U, F> (move).
    template <class U, class F>
    explicit constexpr result(result<U, F>&& other)
        requires(std::is_constructible_v<T, U &&> && std::is_constructible_v<E, F &&>)
      : _has_value(other._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_v) T(cc::move(other._v));
        else
            new (cc::placement_new, &_e) E(cc::move(other._e));
    }

    /// Explicit converting constructor from result<U, F> (copy).
    template <class U, class F>
    explicit constexpr result(result<U, F> const& other)
        requires(std::is_constructible_v<T, U const&> && std::is_constructible_v<E, F const&>)
      : _has_value(other._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_v) T(other._v);
        else
            new (cc::placement_new, &_e) E(other._e);
    }

    /// Implicit conversion to result<T, any_error> via error erasure.
    /// Captures source location at the conversion site to aid debugging.
    template <class F>
    constexpr result(result<T, F>&& other, cc::source_location site = cc::source_location::current())
        requires(std::is_same_v<E, any_error> && !std::is_same_v<F, any_error>)
      : _has_value(other._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_v) T(cc::move(other._v));
        else
            new (cc::placement_new, &_e) any_error(cc::move(other._e), site);
    }

    // special members - trivial when T and E are trivial
public:
    constexpr result(result&& other)
        requires(std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<E>)
    = default;

    constexpr result(result const& other)
        requires(std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<E>)
    = default;

    constexpr result& operator=(result&& other)
        requires(std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<E>)
    = default;

    constexpr result& operator=(result const& other)
        requires(std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<E>)
    = default;

    constexpr ~result()
        requires(std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<E>)
    = default;

    // special members - non-trivial
public:
    constexpr result(result&& other) noexcept
        requires(!std::is_trivially_copyable_v<T> || !std::is_trivially_copyable_v<E>)
      : _has_value(other._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_v) T(cc::move(other._v));
        else
            new (cc::placement_new, &_e) E(cc::move(other._e));
    }

    constexpr result(result const& other)
        requires((!std::is_trivially_copyable_v<T> || !std::is_trivially_copyable_v<E>) //
                 && std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
      : _has_value(other._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_v) T(other._v);
        else
            new (cc::placement_new, &_e) E(other._e);
    }

    constexpr result& operator=(result&& other) noexcept
        requires(!std::is_trivially_copyable_v<T> || !std::is_trivially_copyable_v<E>)
    {
        if (this != &other)
        {
            impl_destroy_active();

            _has_value = other._has_value;
            if (_has_value)
                new (cc::placement_new, &_v) T(cc::move(other._v));
            else
                new (cc::placement_new, &_e) E(cc::move(other._e));
        }
        return *this;
    }

    constexpr result& operator=(result const& other)
        requires((!std::is_trivially_copyable_v<T> || !std::is_trivially_copyable_v<E>) //
                 && std::is_copy_assignable_v<T> && std::is_copy_assignable_v<E>)
    {
        if (this != &other)
        {
            impl_destroy_active();

            _has_value = other._has_value;
            if (_has_value)
                new (cc::placement_new, &_v) T(other._v);
            else
                new (cc::placement_new, &_e) E(other._e);
        }
        return *this;
    }

    constexpr ~result()
        requires(!std::is_trivially_destructible_v<T> || !std::is_trivially_destructible_v<E>)
    {
        impl_destroy_active();
    }

    // observers
public:
    /// Returns true if this result holds a value (success state).
    [[nodiscard]] constexpr bool has_value() const { return _has_value; }

    /// Returns true if this result holds an error (failure state).
    [[nodiscard]] constexpr bool has_error() const { return !_has_value; }

    /// Returns a reference to the held value.
    /// Precondition: has_value() == true.
    /// Uses deducing this (C++23) to forward lvalue/rvalue/const qualifiers.
    [[nodiscard]] constexpr auto&& value(this auto&& self)
    {
        CC_ASSERT(self.has_value(), "attempted to access value of error result");
        return static_cast<decltype(self)&&>(self)._v;
    }

    /// Returns a reference to the held error.
    /// Precondition: has_error() == true.
    /// Uses deducing this (C++23) to forward lvalue/rvalue/const qualifiers.
    [[nodiscard]] constexpr auto&& error(this auto&& self)
    {
        CC_ASSERT(self.has_error(), "attempted to access error of value result");
        return static_cast<decltype(self)&&>(self)._e;
    }

    /// Returns the contained value if present, otherwise returns fallback.
    /// Mirrors std::optional semantics.
    template <class U = std::remove_cv_t<T>>
    [[nodiscard]] constexpr T value_or(this auto&& self, U&& fallback)
    {
        static_assert(std::is_convertible_v<U&&, T>, "fallback must be convertible to T");
        if (self._has_value)
            return static_cast<decltype(self)&&>(self)._v;
        else
            return static_cast<T>(cc::forward<U>(fallback));
    }

    /// Returns the contained error if present, otherwise returns fallback.
    template <class U = std::remove_cv_t<E>>
    [[nodiscard]] constexpr E error_or(this auto&& self, U&& fallback)
    {
        static_assert(std::is_convertible_v<U&&, E>, "fallback must be convertible to E");
        if (!self._has_value)
            return static_cast<decltype(self)&&>(self)._e;
        else
            return static_cast<E>(cc::forward<U>(fallback));
    }

    /// Returns the contained value. Asserts that the value is present
    /// NOTE: This asserts even in release builds.
    [[nodiscard]] constexpr T value_assert(this auto&& self, cc::string_view msg)
    {
        CC_ASSERTS_ALWAYS(self._has_value, msg);
        return static_cast<decltype(self)&&>(self)._v;
    }

    /// Returns the contained error. Asserts that the error is present
    /// NOTE: This asserts even in release builds.
    [[nodiscard]] constexpr E error_assert(this auto&& self, cc::string_view msg)
    {
        CC_ASSERTS_ALWAYS(!self._has_value, msg);
        return static_cast<decltype(self)&&>(self)._e;
    }

    /// Returns the contained value if present, otherwise throws an exception.
    /// If E is any_error, adds context before creating the exception to track the conversion point.
    /// Uses deducing this (C++23) to forward lvalue/rvalue/const qualifiers.
    template <class Exception = cc::result_exception>
    [[nodiscard]] constexpr auto&& or_throw(this auto&& self, cc::source_location site = cc::source_location::current())
    {
        if (self._has_value)
            return static_cast<decltype(self)&&>(self)._v;

        // Add context if E is any_error to track the conversion point
        if constexpr (std::is_same_v<E, any_error>)
        {
            throw Exception::from_error(cc::move(self._e).with_context("converted error result to exception", site));
        }
        else
        {
            throw Exception::from_error(static_cast<decltype(self)&&>(self)._e);
        }
    }

    // mutation
public:
    /// Constructs the value in-place from the given arguments.
    /// Destroys the active member first if necessary.
    /// Returns a reference to the newly constructed value.
    template <class... Args>
    constexpr T& emplace_value(Args&&... args)
    {
        static_assert(std::is_constructible_v<T, Args...>, "T must be constructible from Args...");

        impl_destroy_active();
        new (cc::placement_new, &_v) T(cc::forward<Args>(args)...);
        _has_value = true;
        return _v;
    }

    /// Constructs the error in-place from the given arguments.
    /// Destroys the active member first if necessary.
    /// Returns a reference to the newly constructed error.
    template <class... Args>
    constexpr E& emplace_error(Args&&... args)
    {
        static_assert(std::is_constructible_v<E, Args...>, "E must be constructible from Args...");

        impl_destroy_active();
        new (cc::placement_new, &_e) E(cc::forward<Args>(args)...);
        _has_value = false;
        return _e;
    }

    // context functions (only available when E is any_error)
public:
    /// Add context to the error chain (lvalue version).
    /// Returns *this for chaining.
    /// Only available when E is cc::any_error.
    result& with_context(cc::string message, cc::source_location site = cc::source_location::current()) &
    {
        static_assert(std::is_same_v<E, any_error>, "with_context is only available for result<T, cc::any_error> aka "
                                                    "cc::result<T>. If you need this, use cc::any_error as your error "
                                                    "type.");
        if (!_has_value)
            _e.with_context(cc::move(message), site);
        return *this;
    }

    /// Add context to the error chain (rvalue version).
    /// Returns the moved result for chaining.
    /// Only available when E is cc::any_error.
    result with_context(cc::string message, cc::source_location site = cc::source_location::current()) &&
    {
        static_assert(std::is_same_v<E, any_error>, "with_context is only available for result<T, cc::any_error> aka "
                                                    "cc::result<T>. If you need this, use cc::any_error as your error "
                                                    "type.");
        if (!_has_value)
            _e.with_context(cc::move(message), site);
        return cc::move(*this);
    }

    /// Add context lazily via a callable (lvalue version).
    /// The callable is only invoked if the result contains an error.
    /// Only available when E is cc::any_error.
    template <class Fn>
    result& with_context_lazy(Fn&& fn, cc::source_location site = cc::source_location::current()) &
    {
        static_assert(std::is_same_v<E, any_error>, "with_context_lazy is only available for result<T, cc::any_error> "
                                                    "aka cc::result<T>. If you need this, use cc::any_error as your "
                                                    "error type.");
        if (!_has_value)
            _e.with_context(cc::string(cc::invoke(cc::forward<Fn>(fn))), site);
        return *this;
    }

    /// Add context lazily via a callable (rvalue version).
    /// The callable is only invoked if the result contains an error.
    /// Only available when E is cc::any_error.
    template <class Fn>
    result with_context_lazy(Fn&& fn, cc::source_location site = cc::source_location::current()) &&
    {
        static_assert(std::is_same_v<E, any_error>, "with_context_lazy is only available for result<T, cc::any_error> "
                                                    "aka cc::result<T>. If you need this, use cc::any_error as your "
                                                    "error type.");
        if (!_has_value)
            _e.with_context(cc::string(cc::invoke(cc::forward<Fn>(fn))), site);
        return cc::move(*this);
    }

    // implementation helpers
private:
    constexpr void impl_destroy_active()
    {
        if (_has_value)
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
                _v.~T();
        }
        else
        {
            if constexpr (!std::is_trivially_destructible_v<E>)
                _e.~E();
        }
    }

    // members
private:
    template <class U, class F>
    friend struct result;

    bool _has_value = false;

    union
    {
        T _v;
        E _e;
    };
};

// =========================================================================================================
// CC_RETURN_IF_ERROR - Early return macro for error propagation
// =========================================================================================================

/// Early return macro for propagating errors from cc::result<T, E>.
///
/// Typical usage:
///   auto res = inner_fn();
///   CC_RETURN_IF_ERROR(res).with_context("outer context");
///   // safe: res has value here
///   return res.value();
///
/// When res contains an error, the macro expands to:
///   if (auto&& _cc_res = (res_); _cc_res.has_error())
///       return cc::error(cc::move(_cc_res).error()).with_context("outer context");
///
/// Properties:
/// - Evaluates res exactly once
/// - Allows trailing .with_context("...") chaining on the return statement
/// - No dynamic allocation
/// - Works with result<T, E> to result<U, E> propagation (only error is propagated)
/// - Works with result<T, E> to result<U> (any_error) propagation via implicit conversion
/// - Not else-safe (standard limitation for chainable early-return macros)
///
/// Implementation notes:
/// - Uses forwarding reference to bind to both lvalues and rvalues
/// - Moves the error out via cc::move(_cc_res).error() to handle both move-only and copyable errors
/// - The return type of the containing function must be cc::result<U, F> where F is
///   constructible from E (including implicit conversion to any_error)
///
#define CC_RETURN_IF_ERROR(res_)                      \
    if (auto&& _cc_res = (res_); _cc_res.has_error()) \
    return cc::error(cc::move(_cc_res).error())

// =========================================================================================================
// cc::any_error implementation details
// =========================================================================================================

struct cc::any_error::payload
{
    cc::string message;

    cc::node_allocation<context_node> ctx;
    cc::source_location site;
    cc::node_allocation<cc::stacktrace> trace;

    explicit payload(cc::string msg, cc::source_location s);

    payload(payload const&) = delete;
    payload(payload&&) = delete;
    payload& operator=(payload const&) = delete;
    payload& operator=(payload&&) = delete;
    ~payload();
};
