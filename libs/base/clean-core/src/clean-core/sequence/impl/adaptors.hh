#pragma once

#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/source.hh>

// The single-input adaptor sources behind cc::sequence's transformations: each wraps exactly one source.

namespace cc::impl
{
/// Whether the references a callback F returns outlive the sequence wherever its input's do.
/// A function holding no state can only return references into its arguments or into what outlives it;
/// one with captures may return a reference into them, which dies with the sequence.
template <class F>
inline constexpr bool seq_returns_borrowed = std::is_empty_v<F> || std::is_member_pointer_v<F>
                                          || (std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>);

template <class S, class F>
struct map_source
{
    using inner_item_t = typename S::item_t;
    using item_t = typename seq_item_of<seq_call_result_t<F, inner_item_t>>::type;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S> && (!seq_holds_references<item_t> || seq_returns_borrowed<F>);
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool takes_idx = seq_call_takes_idx<F, inner_item_t>;

    // from the back, the index of the last element is only known with the size
    static constexpr bool is_reversible = seq_has_next_back<S> && (!takes_idx || seq_has_size<S>);

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

    template <class X>
    constexpr item_t _call_back(X&& x, isize idx)
    {
        return impl::seq_call(_f, idx, cc::forward<X>(x));
    }
    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires is_reversible
    {
        isize idx = 0;
        if constexpr (takes_idx)
            idx = _idx + _inner.size() - 1;
        auto r = _inner.next_back();
        if (!r)
            return {};
        return impl::seq_make_next<item_t>([&]() -> item_t { return this->_call_back(impl::seq_get(r), idx); });
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires is_reversible
    {
        isize idx = 0;
        if constexpr (takes_idx)
            idx = _idx + _inner.size() - 1;
        return impl::seq_try_rfold(
            _inner, [&]<class X>(X&& x) -> bool { return step(this->_call_back(cc::forward<X>(x), idx--)); });
    }
};

template <class S, class P>
struct filter_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool takes_idx = seq_call_takes_idx<P, item_t&>;
    static constexpr bool is_reversible = seq_has_next_back<S> && (!takes_idx || seq_has_size<S>);

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

    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires is_reversible
    {
        while (true)
        {
            isize idx = 0;
            if constexpr (takes_idx)
                idx = _idx + _inner.size() - 1;
            auto r = _inner.next_back();
            if (!r || bool(impl::seq_call(_pred, idx, impl::seq_peek(r))))
                return r;
        }
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires is_reversible
    {
        isize idx = 0;
        if constexpr (takes_idx)
            idx = _idx + _inner.size() - 1;
        return impl::seq_try_rfold(_inner,
                                   [&]<class X>(X&& x) -> bool
                                   {
                                       isize i = idx--;
                                       if (!bool(impl::seq_call(_pred, i, x)))
                                           return false;
                                       return step(cc::forward<X>(x));
                                   });
    }
};

/// A source whose first element was pulled to learn it exists, and is held until asked for.
/// This is the one element a runtime non-empty check over a source without is_empty() must buffer.
/// That element may reference storage inside the source (repeat's value, a temporary fixed_vector it owns, a window's
/// buffer), alone or inside a zip's pack, so moving the source carries each reference to the same place in the copy.
/// Constant evaluation cannot follow it there, so a pulled element that references the source is not constexpr.
template <class S>
struct peeked_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_non_empty = true;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _rest;
    seq_next_t<item_t> _first;

    // first, pulled from the source at from, as the same item of the source at to: every reference in it is carried
    static constexpr seq_next_t<item_t> _carry(seq_next_t<item_t>&& first, S const& from, S& to)
    {
        if constexpr (!seq_holds_references<item_t> || seq_is_borrowed<S>)
            return cc::move(first);
        else if constexpr (std::is_reference_v<item_t>)
            return impl::seq_rebase(first, from, to);
        else
        {
            if (!first)
                return {};
            return seq_next_t<item_t>(seq_emplace, [&] { return seq_rebased<item_t>::make(cc::move(*first), from, to); });
        }
    }

    constexpr peeked_source(seq_next_t<item_t>&& first, S&& rest)
      : _rest(cc::move(rest)), _first(peeked_source::_carry(cc::move(first), rest, _rest))
    {
        CC_ASSERT(bool(_first), "a peeked source needs its first element");
    }

    constexpr peeked_source(peeked_source&& rhs) noexcept
      : _rest(cc::move(rhs._rest)), _first(peeked_source::_carry(cc::move(rhs._first), rhs._rest, _rest))
    {
    }
    peeked_source(peeked_source const&) = delete;
    peeked_source& operator=(peeked_source const&) = delete;
    peeked_source& operator=(peeked_source&&) = delete;
    ~peeked_source() = default;

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

    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires seq_has_next_back<S>
    {
        auto r = _rest.next_back();
        if (r || !_first)
            return r;
        auto f = cc::move(_first);
        _first = seq_next_t<item_t>();
        return f;
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

    constexpr take_source(S&& inner, isize n) : _inner(cc::move(inner)), _n(n < 0 ? 0 : n) {}

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

    // an infinite input has no size, and the first n of it have exactly n
    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S> || seq_is_infinite<S>
    {
        if constexpr (seq_is_infinite<S>)
            return _n;
        else
            return impl::seq_min(_n, _inner.size());
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_size<S> || seq_is_infinite<S>
    {
        return this->size() == 0;
    }

    constexpr void truncate(isize n)
        requires seq_has_size<S> || seq_is_infinite<S>
    {
        _n = impl::seq_min(_n, n < 0 ? 0 : n);
    }

    // from the back, the input's elements past the first n are dropped first; the input cannot truncate, or this
    // source would not exist, so that steps them back, evaluating what its next_back() evaluates
    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires seq_has_next_back<S> && seq_has_size<S>
    {
        while (_inner.size() > _n)
            (void)_inner.next_back();
        if (_n <= 0)
            return {};
        --_n;
        return _inner.next_back();
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

    static constexpr bool is_infinite = seq_is_infinite<S>;

    constexpr skip_source(S&& inner, isize n) : _inner(cc::move(inner)), _n(n < 0 ? 0 : n) {}

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

    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        auto const n = _inner.size() - _n;
        return n < 0 ? 0 : n;
    }
    constexpr void truncate(isize n)
        requires seq_has_truncate<S>
    {
        _inner.truncate(_n + impl::seq_min(n, this->size()));
    }

    // from the back, the skipped front is simply never reached
    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires seq_has_next_back<S> && seq_has_size<S>
    {
        if (_inner.size() <= _n)
            return {};
        return _inner.next_back();
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

    static constexpr bool is_infinite = seq_is_infinite<S>;

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
/// A source walked from the back: next() is the input's next_back(), and every positional capability is mirrored.
template <class S>
struct reverse_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool has_pure_at = seq_has_pure_at<S>;

    S _inner;

    constexpr explicit reverse_source(S&& inner) : _inner(cc::move(inner)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next() { return _inner.next_back(); }
    [[nodiscard]] constexpr seq_next_t<item_t> next_back() { return _inner.next(); }
    [[nodiscard]] constexpr bool try_fold(auto&& step) { return impl::seq_try_rfold(_inner, step); }
    [[nodiscard]] constexpr bool try_rfold(auto&& step) { return impl::seq_try_fold(_inner, step); }

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
    template <class V>
        requires seq_has_contains<S, V>
    [[nodiscard]] constexpr bool contains(V const& v) const
    {
        return _inner.contains(v);
    }

    [[nodiscard]] constexpr item_t at(isize i)
        requires seq_has_at<S>
    {
        return _inner.at(_inner.size() - 1 - i);
    }
    constexpr void advance(isize n)
        requires seq_has_truncate<S>
    {
        auto const size = _inner.size();
        _inner.truncate(size - impl::seq_min(n, size));
    }
    constexpr void truncate(isize n)
        requires seq_has_advance<S>
    {
        auto const size = _inner.size();
        _inner.advance(size - impl::seq_min(n, size));
    }
};

/// The input unchanged, with f(idx?, item) called on each element as it passes.
/// It has no size, so count() passes every element by f rather than answering from the input's.
template <class S, class F>
struct inspect_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    F _f;
    isize _idx = 0;

    constexpr inspect_source(S&& inner, F&& f) : _inner(cc::move(inner)), _f(cc::move(f)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        auto r = _inner.next();
        if (r)
            (void)impl::seq_call(_f, _idx, impl::seq_peek(r));
        return r;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      (void)impl::seq_call(_f, _idx, x);
                                      return step(cc::forward<X>(x));
                                  });
    }

    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_is_empty<S>
    {
        return _inner.is_empty();
    }
};

/// The result type a filter_map callback yields per element: a cc::optional<U> holds a U, a U* points at one.
template <class R>
struct seq_filter_map_item;
template <class U>
struct seq_filter_map_item<cc::optional<U>>
{
    using type = U;
};
template <class U>
struct seq_filter_map_item<U*>
{
    using type = U&;
};

/// f(idx?, item) per element, keeping what it returns where that is a value: an engaged optional, or a non-null pointer.
template <class S, class F>
struct filter_map_source
{
    using inner_item_t = typename S::item_t;
    using result_t = std::remove_cvref_t<seq_call_result_t<F, inner_item_t>>;
    using item_t = typename seq_filter_map_item<result_t>::type;
    // a pointer result may point into the input's elements or the callback's state, see seq_returns_borrowed
    static constexpr bool is_borrowed = std::is_reference_v<item_t> && seq_is_borrowed<S> && seq_returns_borrowed<F>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    F _f;
    isize _idx = 0;

    constexpr filter_map_source(S&& inner, F&& f) : _inner(cc::move(inner)), _f(cc::move(f)) {}

    template <class R>
    static constexpr bool _has(R const& r)
    {
        if constexpr (std::is_pointer_v<R>)
            return r != nullptr;
        else
            return r.has_value();
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        while (true)
        {
            auto r = _inner.next();
            if (!r)
                return {};
            auto m = result_t(impl::seq_call(_f, _idx, impl::seq_get(r)));
            if (!filter_map_source::_has(m))
                continue;
            if constexpr (std::is_pointer_v<result_t>)
                return m;
            else
                return seq_next_t<item_t>(seq_emplace, [&] { return cc::move(m).value(); });
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      auto m = result_t(impl::seq_call(_f, _idx, cc::forward<X>(x)));
                                      if (!filter_map_source::_has(m))
                                          return false;
                                      if constexpr (std::is_pointer_v<result_t>)
                                          return step(*m);
                                      else
                                          return step(cc::move(m).value());
                                  });
    }
};

/// The running fold: acc = f(acc, idx?, item) per element, and each acc in turn is an element.
template <class S, class AccT, class F>
struct scan_source
{
    using item_t = AccT;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    AccT _acc;
    F _f;
    isize _idx = 0;

    constexpr scan_source(S&& inner, AccT&& init, F&& f)
      : _inner(cc::move(inner)), _acc(cc::move(init)), _f(cc::move(f))
    {
    }

    template <class X>
    constexpr void _step(X&& x)
    {
        auto f = seq_bind_front<F, AccT>{_f, _acc};
        _acc = AccT(impl::seq_call(f, _idx, cc::forward<X>(x)));
    }

    [[nodiscard]] constexpr seq_slot<AccT> next()
    {
        auto r = _inner.next();
        if (!r)
            return {};
        this->_step(impl::seq_get(r));
        return seq_slot<AccT>(seq_emplace, [&] { return AccT(_acc); });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      this->_step(cc::forward<X>(x));
                                      return step(AccT(_acc));
                                  });
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

/// The identity key, so dedup() compares elements themselves.
struct seq_identity_key
{
    template <class X>
    [[nodiscard]] constexpr X const& operator()(X const& x) const
    {
        return x;
    }
};

/// Each run of consecutive elements with equal key(idx?, item), as its first element.
/// Over borrowed references with the identity key it remembers the last element by pointer;
/// otherwise it keeps a copy of the key, since a reference into the source may die at the next step.
template <class S, class KeyF>
struct dedup_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool keeps_pointer
        = std::is_same_v<KeyF, seq_identity_key> && std::is_reference_v<item_t> && seq_is_borrowed<S>;
    using key_t = std::remove_cvref_t<seq_call_result_t<KeyF, seq_view_t<item_t>>>;
    using last_t = std::conditional_t<keeps_pointer, std::remove_reference_t<item_t>*, seq_slot<key_t>>;

    S _inner;
    KeyF _key;
    last_t _last = {};
    isize _idx = 0;

    constexpr dedup_source(S&& inner, KeyF&& key) : _inner(cc::move(inner)), _key(cc::move(key)) {}

    // whether x starts a new run, remembering it if so
    template <class X>
    constexpr bool _is_new(X& x)
    {
        if constexpr (keeps_pointer)
        {
            if (_last != nullptr && *_last == x)
                return false;
            _last = &x;
            return true;
        }
        else
        {
            auto k = key_t(impl::seq_call(_key, _idx, impl::seq_view(x)));
            if (_last && *_last == k)
                return false;
            _last = seq_slot<key_t>(seq_emplace, [&] { return cc::move(k); });
            return true;
        }
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        while (true)
        {
            auto r = _inner.next();
            if (!r || this->_is_new(impl::seq_peek(r)))
                return r;
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return impl::seq_try_fold(_inner,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      if (!this->_is_new(x))
                                          return false;
                                      return step(cc::forward<X>(x));
                                  });
    }
};

/// Each item with its position, as (isize idx, item): enumerate's source.
/// The position is an offset, so every capability of the input carries over.
template <class S>
struct enumerate_source
{
    using inner_item_t = typename S::item_t;
    using item_t = seq_pack<isize, inner_item_t>;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool has_pure_at = seq_has_pure_at<S>;

    S _inner;
    isize _idx = 0; // the position of the first element left

    constexpr explicit enumerate_source(S&& inner) : _inner(cc::move(inner)) {}

    template <class X>
    static constexpr item_t _pack(isize i, X&& x)
    {
        return impl::seq_make_pack<isize, inner_item_t>(i, cc::forward<X>(x));
    }

    [[nodiscard]] constexpr seq_slot<item_t> next()
    {
        auto r = _inner.next();
        if (!r)
            return {};
        auto const i = _idx++;
        return seq_slot<item_t>(seq_emplace, [&] { return enumerate_source::_pack(i, impl::seq_get(r)); });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if constexpr (seq_has_at<S>)
        {
            auto const n = _inner.size();
            for (isize i = 0; i < n; ++i)
                if (step(enumerate_source::_pack(_idx + i, _inner.at(i))))
                    return true;
            return false;
        }
        else
            return impl::seq_try_fold(_inner, [&]<class X>(X&& x) -> bool
                                      { return step(enumerate_source::_pack(_idx++, cc::forward<X>(x))); });
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

    [[nodiscard]] constexpr item_t at(isize i)
        requires seq_has_at<S>
    {
        return enumerate_source::_pack(_idx + i, _inner.at(i));
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

    [[nodiscard]] constexpr seq_slot<item_t> next_back()
        requires seq_has_next_back<S> && seq_has_size<S>
    {
        auto const i = _idx + _inner.size() - 1;
        auto r = _inner.next_back();
        if (!r)
            return {};
        return seq_slot<item_t>(seq_emplace, [&] { return enumerate_source::_pack(i, impl::seq_get(r)); });
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires seq_has_next_back<S> && seq_has_size<S>
    {
        auto i = _idx + _inner.size() - 1;
        return impl::seq_try_rfold(
            _inner, [&]<class X>(X&& x) -> bool { return step(enumerate_source::_pack(i--, cc::forward<X>(x))); });
    }
};
} // namespace cc::impl
