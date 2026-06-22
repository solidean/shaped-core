#pragma once

#include <clean-core/assert.hh>
#include <clean-core/fwd.hh>

#include <cstring>
#include <initializer_list>
#include <type_traits>

// =========================================================================================================
// Utility functions for common operations
// =========================================================================================================
//
// Move semantics:
//   move(value)                 - cast value to rvalue reference for moving
//   forward<T>(value)           - perfect forwarding for template arguments
//   exchange(obj, new_val)      - replace obj with new_val and return old value
//
// Comparison and clamping:
//   max(a, b)                   - returns the larger of two values (requires operator<)
//   min(a, b)                   - returns the smaller of two values (requires operator<)
//   max({a, b, c, ...})         - returns the largest value in initializer list (requires operator<)
//   min({a, b, c, ...})         - returns the smallest value in initializer list (requires operator<)
//   clamp(v, lo, hi)            - clamps value v to range [lo, hi] (requires operator<)
//
// Wrapping arithmetic:
//   wrapped_increment(pos, max) - increment with wrap-around to 0 at max
//   wrapped_decrement(pos, max) - decrement with wrap-around to max-1 at 0
//
// Integer division:
//   int_div_round_up(nom, denom)           - divide integers and round up (both > 0)
//   int_round_up_to_multiple(val, mult)    - round up value to next multiple
//
// Swapping:
//   swap(a, b)                  - swap values, respects ADL swap overloads
//   swap_by_move(a, b)          - swap without respecting custom overloads
//
// Alignment (value or pointer):
//   is_power_of_two(value)           - check if value is a power of 2
//   align_up(value, alignment)       - increment to next aligned boundary (power of 2)
//   align_down(value, alignment)     - decrement to previous aligned boundary (power of 2)
//   align_up_masked(value, mask)     - increment using pre-computed mask
//   align_down_masked(value, mask)   - decrement using pre-computed mask
//   is_aligned(value, alignment)     - check if aligned at boundary (power of 2)
//
// Callable utilities:
//   unit                                    - empty struct for representing void as a regular value
//   overloaded(f1, f2, ...)                 - combine multiple callables into single overload set
//   void_function                           - callable that returns void for any arguments
//   identify_function                       - callable that returns its argument (identity function)
//   constant_function<C>                    - callable that always returns constant C
//   projection_function<I>                  - callable that returns the I-th argument
//   invoke(f, args...)                      - invoke callable with perfect forwarding (handles member pointers)
//   invoke_with_optional_idx(idx, f, args...) - invoke f, preferring without idx but accepts idx as first arg
//   regular_invoke(f, args...)              - invoke callable, returning unit{} if result is void
//   regular_invoke_with_optional_idx(idx, f, args...) - invoke with optional idx, returning unit{} if result is void
//   is_invocable<F, Args...>                - check if F can be invoked with Args...
//   is_invocable_r<R, F, Args...>           - check if F can be invoked with Args... and result converts to R
//
// Template metaprogramming:
//   dont_deduce<T>                   - disable template argument deduction for T
//   function_ptr<Signature>          - convert function signature to function pointer type
//
// Memory management:
//   new(cc::placement_new, ptr) T    - placement new with explicit tag type
//   storage_for<T>                   - uninitialized storage for manual lifetime management
//   memcpy(dest, src, count)         - copy bytes from src to dest
//
// Scope utilities:
//   CC_DEFER { code }                - execute code at scope-exit (RAII cleanup)
//
// Iterator utilities:
//   sentinel                         - lightweight end-of-range sentinel type
//   begin(c) / end(c)                - get begin/end iterators from container or array
//

// TODO
// - is_memcopyable
// - is/has things

namespace cc
{
// =========================================================================================================
// Move semantics
// =========================================================================================================

/// Cast value to rvalue reference to enable move semantics
/// Indicates that the value can be moved from (its resources can be transferred)
/// Usage:
///   vec.push_back(cc::move(obj));  // transfer obj into vector
///   T b = cc::move(a);              // move construct b from a
template <class T>
[[nodiscard]] CC_FORCE_INLINE constexpr T&& move(T& v) noexcept
{
    return static_cast<T&&>(v);
}

/// Perfect forwarding for template arguments
/// Preserves value category (lvalue/rvalue) when forwarding arguments
/// Usage:
///   template<class T>
///   void wrapper(T&& arg) {
///       foo(cc::forward<T>(arg));  // forwards as lvalue or rvalue depending on T
///   }
template <class T>
[[nodiscard]] CC_FORCE_INLINE constexpr T&& forward(T& v) noexcept
{
    return static_cast<T&&>(v);
}

template <class T>
[[nodiscard]] CC_FORCE_INLINE constexpr T&& forward(T&& v) noexcept // NOLINT
{
    return static_cast<T&&>(v);
}

/// Replace object with new value and return the old value
/// Atomically assigns new_val to obj and returns obj's previous value
/// Usage:
///   int old = cc::exchange(counter, 0);      // reset counter to 0, get old value
///   auto ptr = cc::exchange(p, nullptr);     // take ownership of p, set p to null
///   State prev = cc::exchange(state, State::Ready);  // transition state
template <class T, class U = T>
[[nodiscard]] CC_FORCE_INLINE constexpr T exchange(T& obj, U&& new_val) // NOLINT
{
    T old_val = static_cast<T&&>(obj);
    obj = cc::forward<U>(new_val);
    return old_val;
}

// =========================================================================================================
// Comparison and clamping
// =========================================================================================================

/// Returns the larger of two values using operator<
/// Returns a reference to allow selecting elements without copying (works with noncopyables)
/// Guarantees that cc::min(a,b) and cc::max(a,b) return different elements (never both return the same)
/// When a == b, max returns b (consistent with min returning a)
/// Usage:
///   int const& larger = cc::max(x, y);
///   auto const& obj = cc::max(obj_a, obj_b);  // works with noncopyables
///   cc::max(obj_a, obj_b).foo();              // can call members on result
template <class T>
[[nodiscard]] constexpr T const& max(T const& a, T const& b)
{
    static_assert(requires { a < b; }, "T must support operator<");
    return (b < a) ? a : b; // NOLINT(bugprone-return-const-ref-from-parameter) - returning reference is intentional
}

/// Returns the smaller of two values using operator<
/// Returns a reference to allow selecting elements without copying (works with noncopyables)
/// Guarantees that cc::min(a,b) and cc::max(a,b) return different elements (never both return the same)
/// When a == b, min returns a (consistent with max returning b)
/// Usage:
///   int const& smaller = cc::min(x, y);
///   auto const& obj = cc::min(obj_a, obj_b);  // works with noncopyables
///   cc::min(vec[i], vec[j]).process();        // can call members on result
template <class T>
[[nodiscard]] constexpr T const& min(T const& a, T const& b)
{
    static_assert(requires { a < b; }, "T must support operator<");
    return (b < a) ? b : a; // NOLINT(bugprone-return-const-ref-from-parameter) - returning reference is intentional
}

/// Returns the largest value in an initializer list using operator<
/// Returns a copy (not a reference) since the initializer list is temporary
/// Usage:
///   int largest = cc::max({3, 7, 2, 9, 1});  // returns 9
///   auto val = cc::max({x, y, z});           // returns largest of three values
template <class T>
[[nodiscard]] constexpr T max(std::initializer_list<T> ilist)
{
    static_assert(requires { *ilist.begin() < *ilist.begin(); }, "T must support operator<");
    CC_ASSERT(ilist.size() > 0, "max: initializer list must not be empty");
    auto it = ilist.begin();
    T result = *it++;
    for (; it != ilist.end(); ++it)
    {
        if (result < *it)
            result = *it;
    }
    return result;
}

/// Returns the smallest value in an initializer list using operator<
/// Returns a copy (not a reference) since the initializer list is temporary
/// Usage:
///   int smallest = cc::min({3, 7, 2, 9, 1});  // returns 1
///   auto val = cc::min({x, y, z});            // returns smallest of three values
template <class T>
[[nodiscard]] constexpr T min(std::initializer_list<T> ilist)
{
    static_assert(requires { *ilist.begin() < *ilist.begin(); }, "T must support operator<");
    CC_ASSERT(ilist.size() > 0, "min: initializer list must not be empty");
    auto it = ilist.begin();
    T result = *it++;
    for (; it != ilist.end(); ++it)
    {
        if (*it < result)
            result = *it;
    }
    return result;
}

/// Clamps a value to the range [lo, hi]
/// Returns a reference to one of {v, lo, hi} without copying (works with noncopyables)
/// Precondition: lo <= hi (expressed as !(hi < lo))
/// Usage:
///   int clamped = cc::clamp(x, 0, 100);           // ensures x is in [0, 100]
///   float normalized = cc::clamp(val, 0.0f, 1.0f);
///   cc::clamp(obj, min_obj, max_obj).foo();       // can call members on result
template <class T>
[[nodiscard]] constexpr T const& clamp(T const& v, T const& lo, T const& hi)
{
    static_assert(requires { v < lo; }, "T must support operator<");
    CC_ASSERT(!(hi < lo), "clamp: hi must be >= lo");
    return (v < lo) ? lo : (hi < v) ? hi : v; // NOLINT
}

// =========================================================================================================
// Wrapping arithmetic
// =========================================================================================================

/// Increment with wrap-around: (pos + 1) % max
/// When pos + 1 == max, returns 0; otherwise returns pos + 1
/// Precondition: max > 0
/// Generates optimal assembly for ring buffer increment (no division)
/// Usage:
///   int idx = wrapped_increment(idx, buffer_size);  // wraps at buffer_size
///   // wrapped_increment(0, 3) == 1
///   // wrapped_increment(2, 3) == 0
template <class T>
[[nodiscard]] constexpr T wrapped_increment(T pos, T max)
{
    CC_ASSERT(max > 0, "wrapped_increment: max must be positive");
    ++pos;
    return pos == max ? T(0) : pos;
}

/// Decrement with wrap-around: (pos - 1 + max) % max
/// When pos == 0, returns max - 1; otherwise returns pos - 1
/// Precondition: max > 0
/// Generates optimal assembly for ring buffer decrement (no division)
/// Usage:
///   int idx = wrapped_decrement(idx, buffer_size);  // wraps at 0 to buffer_size-1
///   // wrapped_decrement(1, 3) == 0
///   // wrapped_decrement(0, 3) == 2
template <class T>
[[nodiscard]] constexpr T wrapped_decrement(T pos, T max)
{
    CC_ASSERT(max > 0, "wrapped_decrement: max must be positive");
    return pos == 0 ? max - 1 : pos - 1;
}

// =========================================================================================================
// Integer division
// =========================================================================================================

/// Divide integers and round up: ceil(nom / denom)
/// Precondition: nom > 0 && denom > 0
/// Equivalent to (nom + denom - 1) / denom but avoids overflow
/// Usage:
///   int pages = int_div_round_up(total_items, items_per_page);
///   // int_div_round_up(10, 3) == 4
///   // int_div_round_up(9, 3) == 3
template <class T>
[[nodiscard]] constexpr T int_div_round_up(T nom, T denom)
{
    CC_ASSERT(nom > 0 && denom > 0, "int_div_round_up: both nom and denom must be positive");
    return 1 + ((nom - 1) / denom);
}

/// Round up to the next multiple of a given value
/// Returns the smallest multiple of 'multiple' that is >= val
/// Usage:
///   int aligned = cc::int_round_up_to_multiple(size, 10);  // round to next multiple of 10
///   // cc::int_round_up_to_multiple(23, 10) == 30
///   // cc::int_round_up_to_multiple(30, 10) == 30
/// Corner cases:
///   val == 0: returns 0
///   multiple == 1: returns val
/// Preconditions:
///   multiple > 0
/// Note:
///   For power-of-two multiples, use align_up() instead - it's faster
template <class T>
[[nodiscard]] constexpr T int_round_up_to_multiple(T val, T multiple)
{
    CC_ASSERT(multiple > 0, "int_round_up_to_multiple: multiple must be positive");
    return ((val + multiple - 1) / multiple) * multiple;
}

// =========================================================================================================
// Swapping
// =========================================================================================================

namespace impl
{
struct swap_fn
{
    template <class T>
    constexpr void operator()(T& a, T& b) const;
};
} // namespace impl

/// ADL-aware swap that respects custom swap overloads
/// Implemented as a function object (not a function) so it cannot be found by ADL
/// This allows calling unqualified swap(a, b) inside the implementation to find custom overloads
/// while preventing infinite recursion
/// Usage:
///   cc::swap(a, b);  // finds custom swap via ADL if available, otherwise uses move-based swap
[[maybe_unused]] constexpr impl::swap_fn swap;

/// Simple swap that does not respect custom overloads
/// Always uses move construction/assignment (T must be move-constructible and move-assignable)
/// Use when you explicitly want to bypass custom swap implementations
/// Usage:
///   cc::swap_by_move(a, b);  // always uses T's move operations
template <class T>
constexpr void swap_by_move(T& a, T& b)
{
    T tmp = static_cast<T&&>(a);
    a = static_cast<T&&>(b);
    b = static_cast<T&&>(tmp);
}

// =========================================================================================================
// Alignment (for values or pointers)
// =========================================================================================================

/// Check if a positive value is a power of two
/// Returns true if value is a power of 2 (1, 2, 4, 8, 16, ...)
/// Usage:
///   bool ok = cc::is_power_of_two(256);  // true
///   bool bad = cc::is_power_of_two(100); // false
/// Preconditions:
///   value > 0
template <class T>
[[nodiscard]] constexpr bool is_power_of_two(T value)
{
    CC_ASSERT(value > 0, "is_power_of_two: value must be positive");
    return (value & (value - 1)) == 0;
}

/// Increment value to align at the given pre-computed mask
/// mask should be (alignment - 1) where alignment is a power of 2
/// Faster than align_up when mask is precomputed
/// Usage:
///   auto* aligned = cc::align_up_masked(ptr, 15);  // align to 16 bytes (mask = 16-1 = 15)
///   int aligned_val = cc::align_up_masked(300, 15);  // = 304
template <class T>
[[nodiscard]] constexpr T align_up_masked(T value, isize mask)
{
    return (T)(((isize)value + mask) & ~mask);
}

/// Decrement value to align at the given pre-computed mask
/// mask should be (alignment - 1) where alignment is a power of 2
/// Faster than align_down when mask is precomputed
/// Usage:
///   auto* aligned = cc::align_down_masked(ptr, 15);  // align to 16 bytes (mask = 16-1 = 15)
///   int aligned_val = cc::align_down_masked(300, 15);  // = 288
template <class T>
[[nodiscard]] constexpr T align_down_masked(T value, isize mask)
{
    return (T)((isize)value & ~mask);
}

/// Increment value to align at the given boundary
/// Usage:
///   auto* aligned = cc::align_up(ptr, 256);      // align pointer to 256 bytes
///   int val = cc::align_up(300, 16);             // = 304 (next multiple of 16)
///   // cc::align_up(0x5ACE, 256) == 0x5B00
/// Corner cases:
///   value already aligned: returns value unchanged
///   alignment == 1: returns value unchanged
/// Preconditions:
///   alignment > 0 and alignment must be a power of 2
template <class T>
[[nodiscard]] constexpr T align_up(T value, isize alignment)
{
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "align_up: alignment must be a power of 2");
    return cc::align_up_masked(value, alignment - 1);
}

/// Decrement value to align at the given boundary
/// Usage:
///   auto* aligned = cc::align_down(ptr, 256);    // align pointer to 256 bytes
///   int val = cc::align_down(300, 16);           // = 288 (previous multiple of 16)
///   // cc::align_down(0x5ACE, 256) == 0x5A00
/// Corner cases:
///   value already aligned: returns value unchanged
///   alignment == 1: returns value unchanged
/// Preconditions:
///   alignment > 0 and alignment must be a power of 2
template <class T>
[[nodiscard]] constexpr T align_down(T value, isize alignment)
{
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "align_down: alignment must be a power of 2");
    return cc::align_down_masked(value, alignment - 1);
}

/// Check if value is aligned at the given boundary
/// Usage:
///   if (is_aligned(ptr, 16)) { /* ptr is 16-byte aligned */ }
///   bool ok = is_aligned(size, 4096);  // check if size is page-aligned
/// Corner cases:
///   alignment == 1: always returns true
/// Preconditions:
///   alignment > 0 and alignment must be a power of 2
template <class T>
[[nodiscard]] constexpr bool is_aligned(T value, isize alignment)
{
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "is_aligned: alignment must be a power of 2");
    return 0 == ((isize)value & (alignment - 1));
}

// =========================================================================================================
// Callable utilities
// =========================================================================================================

/// Empty struct representing void as a regular value
/// Allows treating void-returning functions uniformly with value-returning functions
/// Usage:
///   cc::unit do_something() { side_effect(); return {}; }
///   auto result = regular_invoke(f);  // returns unit{} if f returns void
struct unit
{
};

/// Combines multiple callables into a single overload set via variadic inheritance
/// Each callable's operator() becomes available as an overload
/// Usage:
///   auto f = cc::overloaded{
///       [](int x) { return x * 2; },
///       [](float x) { return x * 3.0f; },
///       [](char const* s) { return std::strlen(s); }
///   };
///   f(10);      // calls int overload -> 20
///   f(5.0f);    // calls float overload -> 15.0f
///   f("hello"); // calls string overload -> 5
template <class... Fs>
struct overloaded : Fs...
{
    overloaded(Fs... fs) : Fs(fs)... {}
    using Fs::operator()...;
};

/// Callable that returns void for all possible arguments
/// Useful as a no-op callback or for discarding results
/// Usage:
///   cc::void_function{}();           // returns void
///   cc::void_function{}(1, 2, 3);    // returns void
///   std::visit(cc::void_function{}, variant);  // ignores all alternatives
struct void_function
{
    template <class... Args>
    constexpr void operator()(Args&&...) const noexcept
    {
    }
};

/// Callable that returns its argument with perfect forwarding
/// Preserves value category (lvalue/rvalue) of the input
/// Usage:
///   int x = 5;
///   int& ref = cc::identify_function{}(x);        // returns lvalue reference
///   int&& rval = cc::identify_function{}(10);     // returns rvalue reference
///   auto val = cc::identify_function{}(x);        // copies x
struct identify_function
{
    template <class T>
    constexpr T&& operator()(T&& arg) const noexcept
    {
        return cc::forward<T>(arg);
    }
};

/// Callable that returns a constant value for all possible arguments
/// The constant is returned by value (as auto) regardless of input
/// Usage:
///   cc::constant_function<42>{}();           // returns 42
///   cc::constant_function<42>{}(1, 2, 3);    // returns 42
///   cc::constant_function<3.14f>{}("test");  // returns 3.14f
template <auto C>
struct constant_function
{
    template <class... Args>
    constexpr auto operator()(Args&&...) const noexcept
    {
        return C;
    }
};

/// Callable that returns the I-th argument with perfect forwarding
/// Preserves value category of the selected argument
/// Usage:
///   cc::projection_function<0>{}(10, 20, 30);     // returns first arg: 10
///   cc::projection_function<1>{}(10, 20, 30);     // returns second arg: 20
///   cc::projection_function<2>{}(10, 20, 30);     // returns third arg: 30
template <unsigned I>
struct projection_function
{
    template <class... Args>
    constexpr auto&& operator()(Args&&... args) const noexcept
    {
        return cc::forward<decltype(projection_function::get_nth<I>(cc::forward<Args>(args)...))>(
            projection_function::get_nth<I>(cc::forward<Args>(args)...));
    }

private:
    // TODO: move to real utility
    template <unsigned Idx, class T, class... Ts>
    static constexpr auto&& get_nth(T&& first, Ts&&... rest) noexcept
    {
        if constexpr (Idx == 0)
            return cc::forward<T>(first);
        else
            return projection_function::get_nth<Idx - 1>(cc::forward<Ts>(rest)...);
    }
};

/// Invoke a callable with perfect forwarding, supporting member pointers
/// Handles three cases:
///   - Normal callables: invoke(f, args...) -> f(args...)
///   - Member function pointers: invoke(pmf, obj, args...) -> (obj.*pmf)(args...) or ((*obj).*pmf)(args...)
///   - Member object pointers: invoke(pmd, obj) -> obj.*pmd or (*obj).*pmd
/// The second form ((*obj).*) works with smart pointers and any dereferenceable type
/// Note: Callables with overloaded operator() are fine, but overloaded member function/object pointers
///       are not supported (&Foo::bar must be a unique function/member)
/// Usage:
///   cc::invoke(f);                        // call 0-ary callable
///   cc::invoke(f, x, y);                  // call with args
///   cc::invoke(&Foo::bar, obj, x);        // call member function on object
///   cc::invoke(&Foo::bar, ptr, x);        // call member function via smart pointer
///   cc::invoke(&Foo::member, obj);        // access member object
// 0-ary: only normal callables
template <class F>
constexpr decltype(auto) invoke(F&& f)
{
    if constexpr (requires { cc::forward<F>(f)(); })
    {
        return cc::forward<F>(f)();
    }
    else
    {
        static_assert(false, "cc::invoke(f): f() is not invocable");
    }
}

// 1+ args: member-pointer forms first; otherwise normal call
template <class F, class Arg0, class... Args>
constexpr decltype(auto) invoke(F&& f, Arg0&& arg0, Args&&... args)
{
    using F0 = std::remove_cv_t<std::remove_reference_t<F>>;

    if constexpr (std::is_member_pointer_v<F0>)
    {
        if constexpr (std::is_member_function_pointer_v<F0>)
        {
            if constexpr (requires { (cc::forward<Arg0>(arg0).*f)(cc::forward<Args>(args)...); })
            {
                return (cc::forward<Arg0>(arg0).*f)(cc::forward<Args>(args)...);
            }
            else if constexpr (requires { ((*cc::forward<Arg0>(arg0)).*f)(cc::forward<Args>(args)...); })
            {
                return ((*cc::forward<Arg0>(arg0)).*f)(cc::forward<Args>(args)...);
            }
            else
            {
                static_assert(false, "cc::invoke(pmf, obj, ...): cannot apply .* / ->* and call");
            }
        }
        else // member object pointer
        {
            if constexpr (sizeof...(Args) != 0)
            {
                static_assert(false, "cc::invoke(pmd, obj): member object access takes no extra args");
            }
            else if constexpr (requires { cc::forward<Arg0>(arg0).*f; })
            {
                return cc::forward<Arg0>(arg0).*f;
            }
            else if constexpr (requires { (*cc::forward<Arg0>(arg0)).*f; })
            {
                return (*cc::forward<Arg0>(arg0)).*f;
            }
            else
            {
                static_assert(false, "cc::invoke(pmd, obj): cannot apply .* / ->*");
            }
        }
    }
    else if constexpr (requires { cc::forward<F>(f)(cc::forward<Arg0>(arg0), cc::forward<Args>(args)...); })
    {
        return cc::forward<F>(f)(cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
    }
    else
    {
        static_assert(false, "cc::invoke(f, args...): not invocable (neither member-pointer nor callable)");
    }
}

/// Check if a callable can be invoked with the given arguments
/// Returns a compile-time boolean constant matching the behavior of cc::invoke
/// Usage:
///   cc::is_invocable<decltype(f)>                    // check if f is 0-ary callable
///   cc::is_invocable<decltype(f), int, float>        // check if f(int, float) is valid
///   cc::is_invocable<decltype(&Foo::bar), Foo, int>  // check if member function is invocable
// Implementation details
namespace impl
{
template <class F, class Arg0, class... Args>
consteval bool is_invocable_n()
{
    using F0 = std::remove_cv_t<std::remove_reference_t<F>>;

    if constexpr (std::is_member_pointer_v<F0>)
    {
        if constexpr (std::is_member_function_pointer_v<F0>)
        {
            return requires(F&& f, Arg0&& arg0, Args&&... args) {
                (cc::forward<Arg0>(arg0).*cc::forward<F>(f))(cc::forward<Args>(args)...);
            } || requires(F&& f, Arg0&& arg0, Args&&... args) {
                ((*cc::forward<Arg0>(arg0)).*cc::forward<F>(f))(cc::forward<Args>(args)...);
            };
        }
        else // member object pointer
        {
            if constexpr (sizeof...(Args) != 0)
                return false;

            return requires(F&& f, Arg0&& arg0) { cc::forward<Arg0>(arg0).*cc::forward<F>(f); }
                || requires(F&& f, Arg0&& arg0) { (*cc::forward<Arg0>(arg0)).*cc::forward<F>(f); };
        }
    }
    else
    {
        return requires(F&& f, Arg0&& arg0, Args&&... args) {
            cc::forward<F>(f)(cc::forward<Arg0>(arg0), cc::forward<Args>(args)...);
        };
    }
}

template <class F, class... Args>
consteval bool is_invocable_dispatch()
{
    if constexpr (sizeof...(Args) == 0)
    {
        return requires(F&& f) { cc::forward<F>(f)(); };
    }
    else
    {
        return cc::impl::is_invocable_n<F, Args...>();
    }
}
} // namespace impl

template <class F, class... Args>
inline constexpr bool is_invocable = impl::is_invocable_dispatch<F, Args...>();

/// Check if a callable can be invoked with the given arguments and result converts to R
/// Returns a compile-time boolean constant
/// Special case: is_invocable_r<void, F, Args...> accepts any return type (including void)
/// Usage:
///   cc::is_invocable_r<int, decltype(f)>                 // check if f() returns int (or convertible)
///   cc::is_invocable_r<void, decltype(f), int, float>    // check if f(int, float) is valid (any return)
///   cc::is_invocable_r<float, decltype(&Foo::bar), Foo>  // check if member function returns float
namespace impl
{
template <class R, class F, class... Args>
consteval bool is_invocable_r_impl()
{
    if constexpr (!is_invocable<F, Args...>)
    {
        return false;
    }
    else if constexpr (std::is_void_v<R>)
    {
        // std::is_invocable_r<void, ...> accepts any return type (including void)
        return true;
    }
    else
    {
        using Ret = decltype(cc::invoke(std::declval<F>(), std::declval<Args>()...));
        return std::is_convertible_v<Ret, R>;
    }
}
} // namespace impl

template <class R, class F, class... Args>
inline constexpr bool is_invocable_r = impl::is_invocable_r_impl<R, F, Args...>();

/// Invoke a callable with an optional index argument
/// Tries to invoke F with Args... first; if that fails, tries to invoke F with (Idx, Args...)
/// Prefers the non-indexed form if both work
/// Usage:
///   auto f1 = [](int x) { return x * 2; };
///   auto f2 = [](int idx, int x) { return idx + x; };
///   cc::invoke_with_optional_idx(5, f1, 10);  // calls f1(10), ignores idx
///   cc::invoke_with_optional_idx(5, f2, 10);  // calls f2(5, 10), uses idx
/// Preconditions:
///   F must be invocable with either Args... or (Idx, Args...)
template <class Idx, class F, class... Args>
constexpr decltype(auto) invoke_with_optional_idx(Idx&& idx, F&& f, Args&&... args)
{
    if constexpr (cc::is_invocable<F, Args...>)
    {
        return cc::invoke(cc::forward<F>(f), cc::forward<Args>(args)...);
    }
    else if constexpr (cc::is_invocable<F, Idx, Args...>)
    {
        return cc::invoke(cc::forward<F>(f), cc::forward<Idx>(idx), cc::forward<Args>(args)...);
    }
    else
    {
        static_assert(false, "cc::invoke_with_optional_idx(idx, f, args...): f is not invocable with args... nor with "
                             "(idx, args...)");
    }
}

/// Invoke a callable with perfect forwarding, returning unit{} if the result is void
/// Similar to cc::invoke but converts void results to unit{} for uniform handling
/// Usage:
///   auto r1 = cc::regular_invoke([]{ return 42; });        // r1 is int
///   auto r2 = cc::regular_invoke([]{ side_effect(); });    // r2 is unit
///   // Both can be used uniformly in generic code
template <class F, class... Args>
constexpr auto regular_invoke(F&& f, Args&&... args)
{
    if constexpr (std::is_void_v<decltype(cc::invoke(cc::forward<F>(f), cc::forward<Args>(args)...))>)
    {
        cc::invoke(cc::forward<F>(f), cc::forward<Args>(args)...);
        return unit{};
    }
    else
    {
        return cc::invoke(cc::forward<F>(f), cc::forward<Args>(args)...);
    }
}

/// Invoke a callable with an optional index argument, returning unit{} if the result is void
/// Similar to cc::invoke_with_optional_idx but converts void results to unit{} for uniform handling
/// Usage:
///   auto f1 = [](int x) { side_effect(x); };
///   auto f2 = [](int idx, int x) { return idx + x; };
///   auto r1 = cc::regular_invoke_with_optional_idx(5, f1, 10);  // r1 is unit
///   auto r2 = cc::regular_invoke_with_optional_idx(5, f2, 10);  // r2 is int
template <class Idx, class F, class... Args>
constexpr auto regular_invoke_with_optional_idx(Idx&& idx, F&& f, Args&&... args)
{
    if constexpr (std::is_void_v<decltype(cc::invoke_with_optional_idx(cc::forward<Idx>(idx), cc::forward<F>(f),
                                                                       cc::forward<Args>(args)...))>)
    {
        cc::invoke_with_optional_idx(cc::forward<Idx>(idx), cc::forward<F>(f), cc::forward<Args>(args)...);
        return unit{};
    }
    else
    {
        return cc::invoke_with_optional_idx(cc::forward<Idx>(idx), cc::forward<F>(f), cc::forward<Args>(args)...);
    }
}

// =========================================================================================================
// Template metaprogramming utilities
// =========================================================================================================

namespace impl
{
template <class T>
struct dont_deduce_t
{
    using type = T;
};
} // namespace impl

/// Helper typedef for disabling template argument deduction
/// Prevents the compiler from deducing T in this parameter position
/// See https://artificial-mind.net/blog/2020/09/26/dont-deduce
/// Usage:
///   template <class T>
///   vec3<T> operator*(vec3<T> const& a, cc::dont_deduce<T> b);
///   // Now this works:
///   vec3<float> v = ...;
///   v = v * 3;  // deduces T from first arg only, then 3 is converted to float
template <class T>
using dont_deduce = typename impl::dont_deduce_t<T>::type;

namespace impl
{
template <class T>
struct function_ptr_t
{
    static_assert(false, "function_ptr should only be used with function signatures");
};
template <class R, class... Args>
struct function_ptr_t<R(Args...)>
{
    using type = R (*)(Args...);
};
template <class R, class... Args>
struct function_ptr_t<R(Args...) noexcept>
{
    using type = R (*)(Args...) noexcept;
};
} // namespace impl

/// Type alias for readable function pointer types
/// Converts function signature to function pointer type
/// Usage:
///   cc::function_ptr<int(float, double)>          -> int (*)(float, double)
///   cc::function_ptr<void() noexcept>             -> void (*)() noexcept
///   cc::function_ptr<char*(int)>                  -> char* (*)(int)
template <class T>
using function_ptr = typename impl::function_ptr_t<T>::type;

// =========================================================================================================
// Memory management utilities
// =========================================================================================================

namespace impl
{
struct placement_new_tag
{
};
} // namespace impl

/// Tag object for explicit placement new syntax
/// Provides a more readable alternative to standard placement new
/// Usage:
///   T* ptr = new(cc::placement_new, memory) T();
///   T* arr = new(cc::placement_new, memory) T[5];
[[maybe_unused]] static constexpr impl::placement_new_tag placement_new;

/// Uninitialized storage for a value of type T
/// Provides a union wrapper that does not default-construct or destruct T
/// Useful for manual lifetime management in containers like optional, variant, etc.
/// Usage:
///   template <class T>
///   struct my_optional
///   {
///       void set_value(T t) {
///           if (_has_value)
///               _storage.value.~T();
///           new (&_storage.value) T(t);
///           _has_value = true;
///       }
///
///   private:
///       cc::storage_for<T> _storage;
///       bool _has_value = false;
///   };
template <class T>
union storage_for // NOLINT(cppcoreguidelines-special-member-functions)
{
    // empty dtor in order to not initialize value but preserve triviality
    ~storage_for()
        requires(!std::is_trivially_destructible_v<T>)
    {
    }
    ~storage_for()
        requires std::is_trivially_destructible_v<T>
    = default;

    cc::byte dummy = {};
    T value;
};

/// Copy bytes from source to destination
/// Performs the following operations in order:
///   - Implicitly creates objects at dest.
///   - Copies count characters (as if of type unsigned char) from the object pointed to by src into the object pointed to by dest.
/// If any of the following conditions is satisfied, the behavior is undefined:
///   - dest or src is a null pointer or invalid pointer.
///   - Copying takes place between objects that overlap.
/// Usage:
///   cc::memcpy(dest_ptr, src_ptr, num_bytes);
///   char buffer[100];
///   cc::memcpy(buffer, "hello", 6);
using std::memcpy;

// =========================================================================================================
// Scope utilities
// =========================================================================================================

namespace impl
{
template <class F>
struct deferred
{
    F f;
    explicit deferred(F func) : f(static_cast<F&&>(func)) {}
    ~deferred() noexcept(false) { f(); }

    deferred(deferred const&) = delete;
    deferred& operator=(deferred const&) = delete;
    deferred(deferred&&) = delete;
    deferred& operator=(deferred&&) = delete;
};

struct deferred_tag
{
};

template <class F>
deferred<F> operator+(deferred_tag, F&& f)
{
    return deferred<F>(cc::forward<F>(f));
}
} // namespace impl

/// Execute code at scope-exit (RAII-style cleanup)
/// Captures by reference - be careful with lifetime
/// Usage:
///   begin();
///   CC_DEFER { end(); };
///   // ... code that might throw or return ...
///   // end() is guaranteed to be called when scope exits
#define CC_DEFER auto const CC_MACRO_JOIN(_cc_deferred_, __COUNTER__) = ::cc::impl::deferred_tag{} + [&]

// =========================================================================================================
// Iterator utilities
// =========================================================================================================

/// A generic end-of-range sentinel type
/// Used as a lightweight alternative to a full iterator for range end
/// Usage:
///   struct my_range {
///       my_iterator begin() { return ...; }
///       cc::sentinel end() const { return {}; }
///   };
///   struct my_iterator {
///       bool operator!=(cc::sentinel) const { return is_still_valid(); }
///       // ... other iterator operations ...
///   };
struct sentinel
{
};

/// Get begin iterator from container (mutable version)
/// Calls c.begin() on any container that provides it
/// Usage:
///   std::vector<int> v = {1, 2, 3};
///   auto it = cc::begin(v);  // equivalent to v.begin()
template <class C>
[[nodiscard]] constexpr auto begin(C& c) -> decltype(c.begin())
{
    return c.begin();
}

/// Get begin iterator from container (const version)
/// Calls c.begin() on any const container that provides it
/// Usage:
///   std::vector<int> const& v = ...;
///   auto it = cc::begin(v);  // equivalent to v.begin()
template <class C>
[[nodiscard]] constexpr auto begin(C const& c) -> decltype(c.begin())
{
    return c.begin();
}

/// Get begin iterator from C-style array
/// Returns pointer to first element
/// Usage:
///   int arr[5] = {1, 2, 3, 4, 5};
///   int* it = cc::begin(arr);  // points to arr[0]
template <class T, size_t N>
[[nodiscard]] constexpr T* begin(T (&array)[N]) noexcept
{
    return array;
}

/// Get end iterator from container (mutable version)
/// Calls c.end() on any container that provides it
/// Usage:
///   std::vector<int> v = {1, 2, 3};
///   auto it = cc::end(v);  // equivalent to v.end()
template <class C>
[[nodiscard]] constexpr auto end(C& c) -> decltype(c.end())
{
    return c.end();
}

/// Get end iterator from container (const version)
/// Calls c.end() on any const container that provides it
/// Usage:
///   std::vector<int> const& v = ...;
///   auto it = cc::end(v);  // equivalent to v.end()
template <class C>
[[nodiscard]] constexpr auto end(C const& c) -> decltype(c.end())
{
    return c.end();
}

/// Get end iterator from C-style array
/// Returns pointer past last element
/// Usage:
///   int arr[5] = {1, 2, 3, 4, 5};
///   int* it = cc::end(arr);  // points one past arr[4]
template <class T, size_t N>
[[nodiscard]] constexpr T* end(T (&array)[N]) noexcept
{
    return array + N;
}

} // namespace cc

// =========================================================================================================
// Implementation
// =========================================================================================================

// must be done outside of the cc namespace so cc::swap cannot be found anymore
namespace _no_cc_namespace // NOLINT
{
template <class T>
constexpr void do_swap_impl(T& a, T& b)
{
    if constexpr (requires { swap(a, b); })
    {
        swap(a, b);
    }
    else
    {
        T tmp = static_cast<T&&>(a);
        a = static_cast<T&&>(b);
        b = static_cast<T&&>(tmp);
    }
}
} // namespace _no_cc_namespace

template <class T>
constexpr void cc::impl::swap_fn::operator()(T& a, T& b) const
{
    _no_cc_namespace::do_swap_impl(a, b);
}

// =========================================================================================================
// Global placement new operators for cc::placement_new
// =========================================================================================================

/// Placement new operator for cc::placement_new tag
/// Usage: T* ptr = new(cc::placement_new, memory) T();
constexpr void* operator new(size_t, cc::impl::placement_new_tag, void* buffer)
{
    return buffer;
}

/// Placement new[] operator for cc::placement_new tag
/// Usage: T* arr = new(cc::placement_new, memory) T[5];
constexpr void* operator new[](size_t, cc::impl::placement_new_tag, void* buffer)
{
    return buffer;
}

/// Matching delete operator (required but typically never called)
constexpr void operator delete(void*, cc::impl::placement_new_tag, void*)
{
}
