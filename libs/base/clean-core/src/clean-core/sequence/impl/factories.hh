#pragma once

#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/source.hh>

#include <initializer_list>

// The sources behind the free factories: repeat, generate, iterate, and values_of over a foreign range.

namespace cc::impl
{
/// value, n times, or forever when IsInfinite.
/// Items are references to the one held value, so nothing is copied per step.
template <class T, bool IsInfinite>
struct repeat_source
{
    using item_t = T const&;
    static constexpr bool is_infinite = IsInfinite;
    static constexpr bool is_non_empty = IsInfinite;
    static constexpr bool has_pure_at = !IsInfinite;

    T _value;
    isize _n = 0;

    constexpr explicit repeat_source(T value)
        requires IsInfinite
      : _value(cc::move(value))
    {
    }
    constexpr repeat_source(T value, isize n)
        requires(!IsInfinite)
      : _value(cc::move(value)), _n(n < 0 ? 0 : n)
    {
    }

    [[nodiscard]] constexpr T const* next()
    {
        if constexpr (!IsInfinite)
        {
            if (_n == 0)
                return nullptr;
            --_n;
        }
        return &_value;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if constexpr (IsInfinite)
        {
            while (true)
                if (step(static_cast<T const&>(_value)))
                    return true;
        }
        else
        {
            for (isize i = 0; i < _n; ++i)
                if (step(static_cast<T const&>(_value)))
                    return true;
            return false;
        }
    }

    [[nodiscard]] constexpr isize size() const
        requires(!IsInfinite)
    {
        return _n;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires(!IsInfinite)
    {
        return _n == 0;
    }
    [[nodiscard]] constexpr T const& at(isize) const
        requires(!IsInfinite)
    {
        return _value;
    }
    constexpr void advance(isize n)
        requires(!IsInfinite)
    {
        _n -= impl::seq_min(n, _n);
    }
    constexpr void truncate(isize n)
        requires(!IsInfinite)
    {
        _n = impl::seq_min(n, _n);
    }
    [[nodiscard]] constexpr T const* next_back()
        requires(!IsInfinite)
    {
        return this->next();
    }
};

/// f(), f(), f(), ... forever; each call is one element, made when it is reached.
template <class F>
struct generate_source
{
    using item_t = std::remove_cvref_t<decltype(std::declval<F&>()())>;
    static constexpr bool is_infinite = true;
    static constexpr bool is_non_empty = true;

    F _f;

    constexpr explicit generate_source(F&& f) : _f(cc::move(f)) {}

    [[nodiscard]] constexpr seq_slot<item_t> next()
    {
        return seq_slot<item_t>(seq_emplace, [&] { return _f(); });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        while (true)
            if (step(item_t(_f())))
                return true;
    }
};

/// init, f(init), f(f(init)), ... forever.
/// The next value is computed when it is reached, never ahead, so take(n) calls f n - 1 times.
template <class T, class F>
struct iterate_source
{
    using item_t = T;
    static constexpr bool is_infinite = true;
    static constexpr bool is_non_empty = true;

    T _current;
    F _f;
    bool _started = false;

    constexpr iterate_source(T init, F&& f) : _current(cc::move(init)), _f(cc::move(f)) {}

    constexpr void _step()
    {
        if (_started)
            _current = T(_f(static_cast<T const&>(_current)));
        _started = true;
    }

    [[nodiscard]] constexpr seq_slot<T> next()
    {
        this->_step();
        return seq_slot<T>(seq_emplace, [&] { return T(_current); });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        while (true)
        {
            this->_step();
            if (step(T(_current)))
                return true;
        }
    }
};

/// The source over a begin/end range that is not contiguous, which <clean-core/sequence/iterator_ranges.hh> defines.
/// Telling a forward iterator from an input one needs <iterator>, too expensive for every container to include.
template <class R>
struct seq_iterator_range;

/// Whether a foreign range owns its elements rather than viewing someone else's: std::vector and std::string name an
/// allocator_type, and std::span, std::string_view and the views do not.
template <class R>
inline constexpr bool seq_owns_elements = requires { typename R::allocator_type; };

template <class R>
concept seq_is_contiguous_range = requires(R& r) {
    { r.data() };
    { r.size() };
    requires std::is_pointer_v<decltype(r.data())>;
};

/// The source behind values_of(r) for a range that has no values() of its own.
/// Contiguous storage is a span, borrowed from an lvalue and owned, by index, from a temporary; a C array is borrowed.
/// A foreign temporary is expiring only where it owns its elements, which seq_owns_elements tells by its allocator.
/// Anything else is its begin/end, through <clean-core/sequence/iterator_ranges.hh>.
template <class R>
[[nodiscard]] constexpr auto seq_range_source(R&& r)
{
    using range_t = std::remove_cvref_t<R>;
    constexpr bool is_owned = !std::is_lvalue_reference_v<R>;
    static_assert(
        !is_owned || !std::is_const_v<std::remove_reference_t<R>> || std::is_array_v<std::remove_reference_t<R>>,
        "a sequence over a const temporary: it dies with the expression and cannot be moved from, "
        "so drop the const or bind it to a variable first");
    if constexpr (std::is_array_v<std::remove_reference_t<R>>)
    {
        using element_t = std::remove_extent_t<std::remove_reference_t<R>>;
        constexpr auto extent = isize(std::extent_v<std::remove_reference_t<R>>);
        if constexpr (extent > 0)
            return non_empty_source<span_source<element_t>>(span_source<element_t>(r, r + extent));
        else
            return span_source<element_t>(r, r + extent);
    }
    else if constexpr (seq_is_contiguous_range<std::remove_reference_t<R>>)
    {
        if constexpr (is_owned)
            return owning_source<range_t, seq_owns_elements<range_t>>(cc::move(r));
        else
            return span_source<std::remove_pointer_t<decltype(r.data())>>(r.data(), r.data() + r.size());
    }
    else if constexpr (seq_is_complete<seq_iterator_range<range_t>>)
        return seq_iterator_range<range_t>::make(cc::forward<R>(r));
    else
    {
        static_assert(seq_is_complete<seq_iterator_range<range_t>>,
                      "a sequence over a begin/end range that is not contiguous (std::list, std::map, ...) needs "
                      "<clean-core/sequence/iterator_ranges.hh>");
        return span_source<int>(nullptr, nullptr); // only so the refusal above is the one error
    }
}

/// The source over an initializer list's backing array, which lives only until the end of the full expression:
/// a span, but not borrowed, so no pointer into it escapes the sequence.
template <class T>
struct initializer_list_source : span_source<T const>
{
    static constexpr bool is_borrowed = false;

    constexpr explicit initializer_list_source(std::initializer_list<T> il) : span_source<T const>(il.begin(), il.end())
    {
    }
};

template <class T>
[[nodiscard]] constexpr auto seq_range_source(std::initializer_list<T> il)
{
    return initializer_list_source<T>(il);
}
} // namespace cc::impl
