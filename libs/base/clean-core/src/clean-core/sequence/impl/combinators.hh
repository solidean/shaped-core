#pragma once

#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/impl/factories.hh>
#include <clean-core/sequence/source.hh>

#include <utility>

// The adaptor sources that combine sequences: chain, zip, flatten.

namespace cc::impl
{
template <class T>
inline constexpr bool seq_is_sequence = false;
template <class S>
inline constexpr bool seq_is_sequence<cc::sequence<S>> = true;

/// The source behind x: a sequence's own, the one x.values() hands out, or one over x as a foreign range.
/// A temporary container is owned either way, and one whose values() refuses a temporary is refused here too.
template <class X>
[[nodiscard]] constexpr auto seq_source_of(X&& x)
{
    if constexpr (seq_is_sequence<std::remove_cvref_t<X>>)
    {
        static_assert(!std::is_lvalue_reference_v<X>, "a sequence is consumed, so it must be passed as an rvalue");
        return seq_dependent<X, seq_access>::type::source(cc::move(x));
    }
    else if constexpr (requires { cc::forward<X>(x).values(); })
        return seq_dependent<X, seq_access>::type::source(cc::forward<X>(x).values());
    else
    {
        static_assert(
            !requires(std::remove_cvref_t<X>& c) { c.values(); },
            "this container's values() refuses a temporary, so it cannot be one here either: "
            "bind it to a variable first");
        return impl::seq_range_source(cc::forward<X>(x));
    }
}

/// The item two chained sequences share: the same element type, as a reference where both are one.
/// T& with T const& is T const&; a prvalue on either side makes it a prvalue, copying the other side's elements.
/// Different element types do not chain, since a common type would convert silently: int with unsigned is unsigned.
template <class A, class B>
struct seq_chain_item
{
    static_assert(std::is_same_v<std::remove_cvref_t<A>, std::remove_cvref_t<B>>,
                  "chain() needs both sequences to yield the same element type; map one of them to match first");

    using element_t = std::remove_cvref_t<A>;
    static constexpr bool is_ref = std::is_lvalue_reference_v<A> && std::is_lvalue_reference_v<B>;
    static constexpr bool is_const
        = std::is_const_v<std::remove_reference_t<A>> || std::is_const_v<std::remove_reference_t<B>>;

    using type = std::conditional_t<is_ref, std::conditional_t<is_const, element_t const&, element_t&>, element_t>;
};

/// a, then b.
template <class A, class B>
struct chain_source
{
    using item_t = typename seq_chain_item<typename A::item_t, typename B::item_t>::type;
    static constexpr bool is_non_empty = seq_never_empty<A> || seq_never_empty<B>;
    static constexpr bool is_borrowed = seq_is_borrowed<A> && seq_is_borrowed<B>;
    static constexpr bool is_expiring = seq_is_expiring<A> && seq_is_expiring<B>;
    static constexpr bool is_infinite = seq_is_infinite<A> || seq_is_infinite<B>;

    A _a;
    B _b;
    bool _in_b = false;      // the front has moved past a
    bool _back_in_a = false; // the back has moved past b

    constexpr chain_source(A&& a, B&& b) : _a(cc::move(a)), _b(cc::move(b)) {}

    // one side's next() result as this chain's
    template <class S>
    static constexpr seq_next_t<item_t> _convert(seq_next_t<typename S::item_t>&& r)
    {
        if constexpr (std::is_same_v<typename S::item_t, item_t>)
            return cc::move(r);
        else if constexpr (std::is_reference_v<item_t>)
            return r; // T* to T const*
        else if (!r)
            return {};
        else
            return seq_next_t<item_t>(seq_emplace, [&]() -> item_t { return item_t(impl::seq_get(r)); });
    }

    // one side's step, handing on this chain's item type
    template <class S, class StepF>
    static constexpr bool _fold(S& side, StepF& step)
    {
        if constexpr (std::is_same_v<typename S::item_t, item_t>)
            return impl::seq_try_fold(side, step);
        else
            return impl::seq_try_fold(
                side, [&]<class X>(X&& x) -> bool { return step(static_cast<item_t>(cc::forward<X>(x))); });
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (!_in_b)
        {
            auto r = _a.next();
            if (r)
                return chain_source::_convert<A>(cc::move(r));
            _in_b = true;
        }
        return chain_source::_convert<B>(_b.next());
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (!_in_b && chain_source::_fold(_a, step))
            return true;
        return chain_source::_fold(_b, step);
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires seq_has_next_back<A> && seq_has_next_back<B>
    {
        if (!_back_in_a)
        {
            auto r = _b.next_back();
            if (r)
                return chain_source::_convert<B>(cc::move(r));
            _back_in_a = true;
        }
        return chain_source::_convert<A>(_a.next_back());
    }

    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires seq_has_next_back<A> && seq_has_next_back<B>
    {
        if (!_back_in_a && chain_source::_rfold(_b, step))
            return true;
        return chain_source::_rfold(_a, step);
    }

    template <class S, class StepF>
    static constexpr bool _rfold(S& side, StepF& step)
    {
        if constexpr (std::is_same_v<typename S::item_t, item_t>)
            return impl::seq_try_rfold(side, step);
        else
            return impl::seq_try_rfold(
                side, [&]<class X>(X&& x) -> bool { return step(static_cast<item_t>(cc::forward<X>(x))); });
    }

    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<A> && seq_has_size<B>
    {
        return (_in_b ? 0 : _a.size()) + _b.size();
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_is_empty<A> && seq_has_is_empty<B>
    {
        return (_in_b || _a.is_empty()) && _b.is_empty();
    }

    template <class V>
        requires seq_has_contains<A, V> && seq_has_contains<B, V>
    [[nodiscard]] constexpr bool contains(V const& v) const
    {
        return _a.contains(v) || _b.contains(v);
    }

    constexpr void truncate(isize n)
        requires seq_has_truncate<A> && seq_has_truncate<B>
    {
        auto const in_a = _in_b ? 0 : _a.size();
        if (n < in_a)
        {
            _a.truncate(n);
            _b.truncate(0);
        }
        else
            _b.truncate(n - in_a);
    }
};

/// One item of each source per step, as a pack, until the shortest ends.
/// Sources that all know their size and element positions run as one index loop; otherwise the first drives
/// internally and the rest step externally.
template <class S0, class... Ss>
struct zip_source
{
    using item_t = seq_pack<typename S0::item_t, typename Ss::item_t...>;
    static constexpr bool is_non_empty = seq_never_empty<S0> && (seq_never_empty<Ss> && ...);
    static constexpr bool is_borrowed = seq_is_borrowed<S0> && (seq_is_borrowed<Ss> && ...);
    static constexpr bool is_indexed = seq_has_at<S0> && (seq_has_at<Ss> && ...);
    static constexpr bool has_pure_at = seq_has_pure_at<S0> && (seq_has_pure_at<Ss> && ...);
    static constexpr bool is_sized = seq_has_size<S0> && (seq_has_size<Ss> && ...);
    static constexpr bool is_infinite = seq_is_infinite<S0> && (seq_is_infinite<Ss> && ...);
    static constexpr bool is_reversible = is_sized && seq_has_next_back<S0> && (seq_has_next_back<Ss> && ...);

    using rest_index_t = std::index_sequence_for<Ss...>;

    S0 _first;
    seq_pack<Ss...> _rest;

    constexpr zip_source(S0&& first, Ss&&... rest)
      : _first(cc::move(first)), _rest(impl::seq_make_pack<Ss...>(cc::move(rest)...))
    {
    }

    template <std::size_t I>
    constexpr auto& _r()
    {
        return _rest.template get<I>();
    }
    template <std::size_t I>
    constexpr auto const& _r() const
    {
        return _rest.template get<I>();
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> seq_next_t<item_t>
        {
            auto r0 = _first.next();
            if (!r0)
                return {};
            auto rs = impl::seq_make_pack<seq_next_t<typename Ss::item_t>...>(this->template _r<Is>().next()...);
            if (!(bool(rs.template get<Is>()) && ...))
                return {};
            return seq_next_t<item_t>(seq_emplace,
                                      [&]
                                      {
                                          return impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(
                                              impl::seq_get(r0), impl::seq_get(rs.template get<Is>())...);
                                      });
        }(rest_index_t{});
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> bool
        {
            if constexpr (is_indexed)
            {
                isize n = _first.size();
                ((n = impl::seq_min(n, this->template _r<Is>().size())), ...);
                for (isize i = 0; i < n; ++i)
                    if (step(impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(
                            _first.at(i), this->template _r<Is>().at(i)...)))
                        return true;
                return false;
            }
            else
            {
                bool exhausted = false;
                bool const stopped = impl::seq_try_fold(
                    _first,
                    [&]<class X>(X&& x) -> bool
                    {
                        auto rs
                            = impl::seq_make_pack<seq_next_t<typename Ss::item_t>...>(this->template _r<Is>().next()...);
                        if (!(bool(rs.template get<Is>()) && ...))
                        {
                            exhausted = true;
                            return true;
                        }
                        return step(impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(
                            cc::forward<X>(x), impl::seq_get(rs.template get<Is>())...));
                    });
                return stopped && !exhausted;
            }
        }(rest_index_t{});
    }

    [[nodiscard]] constexpr isize size() const
        requires is_sized
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            isize n = _first.size();
            ((n = impl::seq_min(n, this->template _r<Is>().size())), ...);
            return n;
        }(rest_index_t{});
    }

    // from the back over positions: one descending index loop
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires is_indexed
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> bool
        {
            for (isize i = this->size() - 1; i >= 0; --i)
                if (step(impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(
                        _first.at(i), this->template _r<Is>().at(i)...)))
                    return true;
            return false;
        }(rest_index_t{});
    }

    // a side cut to n elements: truncated where it can be, otherwise stepped back, which evaluates what its next_back()
    // evaluates
    template <class Side>
    static constexpr void _trim(Side& side, isize n)
    {
        if constexpr (seq_has_truncate<Side>)
            side.truncate(n);
        else
            while (side.size() > n)
                (void)side.next_back();
    }

    // from the back, every side first drops what reaches past the shortest
    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires is_reversible
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> seq_next_t<item_t>
        {
            auto const n = this->size();
            zip_source::_trim(_first, n);
            (zip_source::_trim(this->template _r<Is>(), n), ...);
            if (n == 0)
                return {};
            auto r0 = _first.next_back();
            auto rs = impl::seq_make_pack<seq_next_t<typename Ss::item_t>...>(this->template _r<Is>().next_back()...);
            return seq_next_t<item_t>(seq_emplace,
                                      [&]
                                      {
                                          return impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(
                                              impl::seq_get(r0), impl::seq_get(rs.template get<Is>())...);
                                      });
        }(rest_index_t{});
    }

    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return impl::seq_make_pack<typename S0::item_t, typename Ss::item_t...>(_first.at(i),
                                                                                    this->template _r<Is>().at(i)...);
        }(rest_index_t{});
    }
    constexpr void advance(isize n)
        requires seq_has_advance<S0> && (seq_has_advance<Ss> && ...)
    {
        [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            _first.advance(n);
            (this->template _r<Is>().advance(n), ...);
        }(rest_index_t{});
    }
    constexpr void truncate(isize n)
        requires seq_has_truncate<S0> && (seq_has_truncate<Ss> && ...)
    {
        [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            _first.truncate(n);
            (this->template _r<Is>().truncate(n), ...);
        }(rest_index_t{});
    }
};

/// The elements of each element: each item is a sequence, or a container whose values() is taken.
/// It reverses when the outer sequence and the inner ones do: the back keeps an inner source of its own.
template <class S>
struct flatten_source
{
    using outer_item_t = typename S::item_t;
    using inner_source_t = decltype(impl::seq_source_of(std::declval<outer_item_t>()));
    using item_t = typename inner_source_t::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S> && seq_is_borrowed<inner_source_t>;
    // an infinite outer may hold only empty inners, which never ends either; an infinite first inner never ends
    static constexpr bool is_infinite = seq_is_infinite<S> || (seq_never_empty<S> && seq_is_infinite<inner_source_t>);
    static constexpr bool is_reversible = seq_has_next_back<S> && seq_has_next_back<inner_source_t>;

    S _outer;
    seq_slot<inner_source_t> _current; // the inner source the front is in
    seq_slot<inner_source_t> _back;    // the inner source the back is in

    constexpr explicit flatten_source(S&& outer) : _outer(cc::move(outer)) {}

    template <class O>
    static constexpr seq_slot<inner_source_t> _open(O& o)
    {
        return seq_slot<inner_source_t>(seq_emplace, [&] { return impl::seq_source_of(impl::seq_get(o)); });
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        while (true)
        {
            if (_current)
            {
                auto r = (*_current).next();
                if (r)
                    return r;
            }
            auto o = _outer.next();
            if (!o)
            {
                // the outer sequence is used up, so whatever the back opened is all that is left
                if (_back)
                    return (*_back).next();
                return {};
            }
            _current = flatten_source::_open(o);
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (_current && impl::seq_try_fold(*_current, step))
            return true;
        if (impl::seq_try_fold(_outer,
                               [&]<class X>(X&& x) -> bool
                               {
                                   auto inner = impl::seq_source_of(cc::forward<X>(x));
                                   return impl::seq_try_fold(inner, step);
                               }))
            return true;
        return _back && impl::seq_try_fold(*_back, step);
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires is_reversible
    {
        while (true)
        {
            if (_back)
            {
                auto r = (*_back).next_back();
                if (r)
                    return r;
            }
            auto o = _outer.next_back();
            if (!o)
            {
                if (_current)
                    return (*_current).next_back();
                return {};
            }
            _back = flatten_source::_open(o);
        }
    }

    [[nodiscard]] constexpr bool try_rfold(auto&& step)
        requires is_reversible
    {
        if (_back && impl::seq_try_rfold(*_back, step))
            return true;
        if (impl::seq_try_rfold(_outer,
                                [&]<class X>(X&& x) -> bool
                                {
                                    auto inner = impl::seq_source_of(cc::forward<X>(x));
                                    return impl::seq_try_rfold(inner, step);
                                }))
            return true;
        return _current && impl::seq_try_rfold(*_current, step);
    }
};
} // namespace cc::impl
