#pragma once

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

/// A foreign begin/end range: whatever *it yields, a reference or a prvalue.
template <class It, class End>
struct iterator_source
{
    using deref_t = decltype(*std::declval<It&>());
    using item_t = std::conditional_t<std::is_lvalue_reference_v<deref_t>, deref_t, std::remove_cvref_t<deref_t>>;
    static constexpr bool is_borrowed = true;
    static constexpr bool is_random_access = requires(It a, End b, isize i) {
        { b - a };
        a[i];
        a += i;
    };
    static constexpr bool has_pure_at = is_random_access && std::is_lvalue_reference_v<deref_t>;

    It _it;
    End _end;

    constexpr iterator_source(It it, End end) : _it(cc::move(it)), _end(cc::move(end)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (_it == _end)
            return {};
        if constexpr (std::is_lvalue_reference_v<item_t>)
        {
            auto* const p = &*_it;
            ++_it;
            return p;
        }
        else
        {
            auto r = seq_next_t<item_t>(seq_emplace, [&] { return item_t(*_it); });
            ++_it;
            return r;
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        for (; _it != _end; ++_it)
            if (step(static_cast<item_t>(*_it)))
                return true;
        return false;
    }

    [[nodiscard]] constexpr auto begin() const { return _it; }
    [[nodiscard]] constexpr auto end() const { return _end; }

    [[nodiscard]] constexpr isize size() const
        requires is_random_access
    {
        return isize(_end - _it);
    }
    [[nodiscard]] constexpr bool is_empty() const { return _it == _end; }
    [[nodiscard]] constexpr item_t at(isize i) const
        requires is_random_access
    {
        return _it[i];
    }
    constexpr void advance(isize n)
        requires is_random_access
    {
        _it += impl::seq_min(n, this->size());
    }
};

/// A temporary foreign range that is not contiguous, owned through one heap allocation.
/// Its iterators may point into the range object itself (std::list keeps its end sentinel there), so the range must not
/// move once they exist; the allocation is what lets the source move instead.
template <class R>
struct owning_range_source
{
    using it_t = decltype(std::declval<R&>().begin());
    using end_t = decltype(std::declval<R&>().end());
    using inner_t = iterator_source<it_t, end_t>;
    using item_t = typename inner_t::item_t;
    static constexpr bool is_expiring = std::is_lvalue_reference_v<item_t>;

    R* _range;
    inner_t _inner;

    constexpr explicit owning_range_source(R&& r) : _range(new R(cc::move(r))), _inner(_range->begin(), _range->end())
    {
    }

    constexpr owning_range_source(owning_range_source&& rhs) noexcept : _range(rhs._range), _inner(cc::move(rhs._inner))
    {
        rhs._range = nullptr;
    }
    owning_range_source(owning_range_source const&) = delete;
    owning_range_source& operator=(owning_range_source const&) = delete;
    owning_range_source& operator=(owning_range_source&&) = delete;
    constexpr ~owning_range_source() { delete _range; }

    [[nodiscard]] constexpr auto next() { return _inner.next(); }
    [[nodiscard]] constexpr bool try_fold(auto&& step) { return _inner.try_fold(step); }
    [[nodiscard]] constexpr bool is_empty() const { return _inner.is_empty(); }
    [[nodiscard]] constexpr isize size() const
        requires inner_t::is_random_access
    {
        return _inner.size();
    }
};

template <class R>
concept seq_is_contiguous_range = requires(R& r) {
    { r.data() };
    { r.size() };
    requires std::is_pointer_v<decltype(r.data())>;
};

/// The source behind values_of(r) for a range that has no values() of its own.
/// Contiguous storage is a span, borrowed from an lvalue and owned, by index, from a temporary; a C array is borrowed.
/// Anything else is its begin/end, and a temporary is owned through one allocation.
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
            return owning_source<range_t>(cc::move(r));
        else
            return span_source<std::remove_pointer_t<decltype(r.data())>>(r.data(), r.data() + r.size());
    }
    else if constexpr (is_owned)
        return owning_range_source<range_t>(cc::move(r));
    else
        return iterator_source<decltype(r.begin()), decltype(r.end())>(r.begin(), r.end());
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
