#pragma once

#include <clean-core/sequence/source.hh>

// The adaptor sources behind cc::sequence's transformations: each wraps exactly one source.

namespace cc::impl
{
/// Calls f(item) or f(idx, item), counting positions only when f takes the index.
/// The index is the item's position in the sequence f is applied to.
template <class F>
struct seq_indexed_call
{
    F& f;
    isize idx = 0;

    template <class X>
    constexpr decltype(auto) operator()(X&& x)
    {
        if constexpr (cc::is_invocable<F&, X&&>)
            return cc::invoke(f, cc::forward<X>(x));
        else
        {
            static_assert(cc::is_invocable<F&, isize, X&&>, "the callback takes neither (item) nor (isize idx, item)");
            return cc::invoke(f, idx++, cc::forward<X>(x));
        }
    }
};

/// Whether a step result stops the fold: void never stops, anything else stops when true.
template <class F, class... Args>
[[nodiscard]] constexpr bool seq_stops(F&& f, Args&&... args)
{
    if constexpr (std::is_void_v<decltype(cc::forward<F>(f)(cc::forward<Args>(args)...))>)
    {
        cc::forward<F>(f)(cc::forward<Args>(args)...);
        return false;
    }
    else
        return bool(cc::forward<F>(f)(cc::forward<Args>(args)...));
}

/// acc += x where the type has it, acc = acc + x where it has only +.
template <class AccT, class X>
constexpr void seq_add_to(AccT& acc, X const& x)
{
    if constexpr (requires { acc += x; })
        acc += x;
    else
        acc = acc + x;
}

/// A callback result as a sequence item: an lvalue reference is kept, anything else is a prvalue.
/// An rvalue reference decays too, since a pointer could not carry its value category through next().
template <class R>
struct seq_item_of
{
    using type = std::remove_cv_t<R>;
};
template <class R>
struct seq_item_of<R&>
{
    using type = R&;
};
template <class R>
struct seq_item_of<R&&>
{
    using type = std::remove_cv_t<R>;
};

template <class F, class ItemT>
using seq_mapped_item_t =
    typename seq_item_of<decltype(cc::invoke_with_optional_idx(isize(0), std::declval<F&>(), std::declval<ItemT>()))>::type;

template <class S, class F>
struct map_source
{
    using inner_item_t = typename S::item_t;
    using item_t = seq_mapped_item_t<F, inner_item_t>;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;

    S _inner;
    F _f;
    isize _idx = 0;

    constexpr map_source(S&& inner, F&& f) : _inner(cc::move(inner)), _f(cc::move(f)) {}

    template <class X>
    constexpr item_t _call(X&& x)
    {
        if constexpr (cc::is_invocable<F&, X&&>)
            return cc::invoke(_f, cc::forward<X>(x));
        else
            return cc::invoke(_f, _idx++, cc::forward<X>(x));
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        auto r = _inner.next();
        if (!r)
            return {};
        if constexpr (std::is_reference_v<item_t>)
            return &this->_call(impl::seq_get(r));
        else
            return seq_next_t<item_t>(seq_emplace, [&]() -> item_t { return this->_call(impl::seq_get(r)); });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner, [&]<class X>(X&& x) -> bool { return step(this->_call(cc::forward<X>(x))); });
    }

    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        return _inner.size();
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_is_empty<S>
    {
        return _inner.is_empty();
    }
};

template <class S, class P>
struct filter_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    S _inner;
    P _pred;
    isize _idx = 0;

    constexpr filter_source(S&& inner, P&& pred) : _inner(cc::move(inner)), _pred(cc::move(pred)) {}

    template <class X>
    constexpr bool _test(X& x)
    {
        if constexpr (cc::is_invocable<P&, X&>)
            return bool(cc::invoke(_pred, x));
        else
            return bool(cc::invoke(_pred, _idx++, x));
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        while (true)
        {
            auto r = _inner.next();
            if (!r || this->_test(impl::seq_peek(r)))
                return r;
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      if (!this->_test(x))
                                          return false;
                                      return step(cc::forward<X>(x));
                                  });
    }

    // a value is in the filtered sequence iff it is in the input and passes
    template <class V>
        requires seq_has_contains<S, V> && cc::is_invocable<P const&, V const&>
    [[nodiscard]] constexpr bool contains(V const& v) const
    {
        return _inner.contains(v) && bool(cc::invoke(_pred, v));
    }
};

/// A source whose first element was pulled to learn it exists, and is held until asked for.
/// This is the one element a runtime non-empty check over a source without is_empty() must buffer.
template <class S>
struct peeked_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_non_empty = true;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    seq_next_t<item_t> _first;
    S _rest;

    constexpr peeked_source(seq_next_t<item_t>&& first, S&& rest) : _first(cc::move(first)), _rest(cc::move(rest))
    {
        CC_ASSERT(bool(_first), "a peeked source needs its first element");
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (!_first)
            return _rest.next();
        auto r = cc::move(_first);
        _first = seq_next_t<item_t>();
        return r;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (_first && step(impl::seq_get(_first)))
            return true;
        return impl::seq_try_fold(_rest, step);
    }

    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        return _rest.size() + (_first ? 1 : 0);
    }
};
} // namespace cc::impl
