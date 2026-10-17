#pragma once

#include <clean-core/container/tuple.hh>
#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/source.hh>

#include <utility>

// The adaptor sources that combine sequences: chain, zip, flatten.

namespace cc::impl
{
template <class T>
inline constexpr bool seq_is_sequence = false;
template <class S>
inline constexpr bool seq_is_sequence<cc::sequence<S>> = true;

/// The source behind x: a sequence's own, or the one x.values() hands out, owning x when it is a temporary.
template <class X>
[[nodiscard]] constexpr auto seq_source_of(X&& x)
{
    if constexpr (seq_is_sequence<std::remove_cvref_t<X>>)
    {
        static_assert(!std::is_lvalue_reference_v<X>, "a sequence is consumed, so it must be passed as an rvalue");
        return seq_dependent<X, seq_access>::type::source(cc::move(x));
    }
    else
        return seq_dependent<X, seq_access>::type::source(cc::forward<X>(x).values());
}

/// a, then b.
template <class A, class B>
struct chain_source
{
    static_assert(std::is_same_v<typename A::item_t, typename B::item_t>,
                  "chain() needs both sequences to yield the same item type; map one of them to match first");

    using item_t = typename A::item_t;
    static constexpr bool is_non_empty = seq_is_non_empty<A> || seq_is_non_empty<B>;
    static constexpr bool is_borrowed = seq_is_borrowed<A> && seq_is_borrowed<B>;
    static constexpr bool is_expiring = seq_is_expiring<A> && seq_is_expiring<B>;

    A _a;
    B _b;
    bool _in_b = false;

    constexpr chain_source(A&& a, B&& b) : _a(cc::move(a)), _b(cc::move(b)) {}

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if (!_in_b)
        {
            auto r = _a.next();
            if (r)
                return r;
            _in_b = true;
        }
        return _b.next();
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (!_in_b && impl::seq_try_fold(_a, step))
            return true;
        return impl::seq_try_fold(_b, step);
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
};

/// One item of each source per step, as a pack, until the shortest ends.
/// Sources that all know their size and element positions run as one index loop; otherwise the first drives
/// internally and the rest step externally.
template <class S0, class... Ss>
struct zip_source
{
    using item_t = seq_pack<typename S0::item_t, typename Ss::item_t...>;
    static constexpr bool is_non_empty = seq_is_non_empty<S0> && (seq_is_non_empty<Ss> && ...);
    static constexpr bool is_borrowed = seq_is_borrowed<S0> && (seq_is_borrowed<Ss> && ...);
    static constexpr bool is_indexed = seq_has_at<S0> && (seq_has_at<Ss> && ...);
    static constexpr bool is_sized = seq_has_size<S0> && (seq_has_size<Ss> && ...);

    using rest_index_t = std::index_sequence_for<Ss...>;

    S0 _first;
    cc::tuple<Ss...> _rest;

    constexpr zip_source(S0&& first, Ss&&... rest) : _first(cc::move(first)), _rest(cc::move(rest)...) {}

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
            auto rs = cc::tuple<seq_next_t<typename Ss::item_t>...>(this->template _r<Is>().next()...);
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
                        auto rs = cc::tuple<seq_next_t<typename Ss::item_t>...>(this->template _r<Is>().next()...);
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
template <class S>
struct flatten_source
{
    using outer_item_t = typename S::item_t;
    using inner_source_t = decltype(impl::seq_source_of(std::declval<outer_item_t>()));
    using item_t = typename inner_source_t::item_t;
    static constexpr bool is_borrowed = seq_is_borrowed<S> && seq_is_borrowed<inner_source_t>;

    S _outer;
    seq_slot<inner_source_t> _current;

    constexpr explicit flatten_source(S&& outer) : _outer(cc::move(outer)) {}

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
                return {};
            _current = seq_slot<inner_source_t>(seq_emplace, [&] { return impl::seq_source_of(impl::seq_get(o)); });
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if (_current && impl::seq_try_fold(*_current, step))
            return true;
        return impl::seq_try_fold(_outer,
                                  [&]<class X>(X&& x) -> bool
                                  {
                                      auto inner = impl::seq_source_of(cc::forward<X>(x));
                                      return impl::seq_try_fold(inner, step);
                                  });
    }
};
} // namespace cc::impl
