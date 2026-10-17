#pragma once

#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/source.hh>

// The single-input adaptor sources behind cc::sequence's transformations: each wraps exactly one source.

namespace cc::impl
{
template <class S, class F>
struct map_source
{
    using inner_item_t = typename S::item_t;
    using item_t = typename seq_item_of<seq_call_result_t<F, inner_item_t>>::type;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;

    S _inner;
    F _f;
    isize _idx = 0;

    constexpr map_source(S&& inner, F&& f) : _inner(cc::move(inner)), _f(cc::move(f)) {}

    template <class X>
    constexpr item_t _call(X&& x)
    {
        return impl::seq_call(_f, _idx, cc::forward<X>(x));
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        auto r = _inner.next();
        if (!r)
            return {};
        return impl::seq_make_next<item_t>([&]() -> item_t { return this->_call(impl::seq_get(r)); });
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

    // the callback's index is the position in the input, so it stays _idx + i
    [[nodiscard]] constexpr item_t at(isize i)
        requires seq_has_at<S>
    {
        isize idx = _idx + i;
        return impl::seq_call(_f, idx, _inner.at(i));
    }
    constexpr void advance(isize n)
        requires seq_has_advance<S>
    {
        auto const k = impl::seq_min(n, _inner.size());
        _inner.advance(k);
        _idx += k;
    }
    constexpr void truncate(isize n)
        requires seq_has_truncate<S>
    {
        _inner.truncate(n);
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
        return bool(impl::seq_call(_pred, _idx, x));
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

/// The first n elements of a source that cannot truncate itself.
template <class S>
struct take_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    S _inner;
    isize _n;

    constexpr take_source(S&& inner, isize n) : _inner(cc::move(inner)), _n(n) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (_n <= 0)
            return {};
        --_n;
        return _inner.next();
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (_n <= 0)
            return false;
        bool stopped = false;
        (void)impl::seq_try_fold(_inner,
                                 [&]<class X>(X&& x) -> bool
                                 {
                                     if (step(cc::forward<X>(x)))
                                     {
                                         stopped = true;
                                         return true;
                                     }
                                     return --_n == 0;
                                 });
        return stopped;
    }

    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        return impl::seq_min(_n < 0 ? 0 : _n, _inner.size());
    }
};

/// A source past its first n elements, for one that cannot advance itself: the skipped ones are still evaluated.
template <class S>
struct skip_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    S _inner;
    isize _n;

    constexpr skip_source(S&& inner, isize n) : _inner(cc::move(inner)), _n(n) {}

    constexpr void _skip()
    {
        for (; _n > 0; --_n)
            if (!_inner.next())
            {
                _n = 0;
                return;
            }
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        this->_skip();
        return _inner.next();
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        this->_skip();
        return impl::seq_try_fold(_inner, step);
    }
};

/// The elements before the first that fails pred.
template <class S, class P>
struct take_while_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    S _inner;
    P _pred;
    isize _idx = 0;
    bool _done = false;

    constexpr take_while_source(S&& inner, P&& pred) : _inner(cc::move(inner)), _pred(cc::move(pred)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (_done)
            return {};
        auto r = _inner.next();
        if (r && !bool(impl::seq_call(_pred, _idx, impl::seq_peek(r))))
        {
            _done = true;
            return {};
        }
        return r;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (_done)
            return false;
        bool stopped = false;
        (void)impl::seq_try_fold(_inner,
                                 [&]<class X>(X&& x) -> bool
                                 {
                                     if (!bool(impl::seq_call(_pred, _idx, x)))
                                         return true;
                                     if (step(cc::forward<X>(x)))
                                     {
                                         stopped = true;
                                         return true;
                                     }
                                     return false;
                                 });
        return stopped;
    }
};

/// The elements from the first that fails pred on.
template <class S, class P>
struct skip_while_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;

    S _inner;
    P _pred;
    isize _idx = 0;
    bool _skipping = true;

    constexpr skip_while_source(S&& inner, P&& pred) : _inner(cc::move(inner)), _pred(cc::move(pred)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        while (true)
        {
            auto r = _inner.next();
            if (!r || !_skipping || !bool(impl::seq_call(_pred, _idx, impl::seq_peek(r))))
            {
                _skipping = false;
                return r;
            }
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      if (_skipping)
                                      {
                                          if (bool(impl::seq_call(_pred, _idx, x)))
                                              return false;
                                          _skipping = false;
                                      }
                                      return step(cc::forward<X>(x));
                                  });
    }
};
} // namespace cc::impl
