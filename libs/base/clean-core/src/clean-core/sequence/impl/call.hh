#pragma once

#include <clean-core/sequence/source.hh>

#include <utility>

// How a sequence calls a user callback: the optional leading index, and a zip's items spread into arguments.

namespace cc::impl
{
template <std::size_t I, class T>
struct seq_pack_leaf
{
    T value;
};

/// The I-th type of Ts, without <tuple>.
template <std::size_t I, class T, class... Ts>
struct seq_type_at
{
    using type = typename seq_type_at<I - 1, Ts...>::type;
};
template <class T, class... Ts>
struct seq_type_at<0, T, Ts...>
{
    using type = T;
};

template <class IndexSeqT, class... Ts>
struct seq_pack_storage;

template <std::size_t... Is, class... Ts>
struct seq_pack_storage<std::index_sequence<Is...>, Ts...> : seq_pack_leaf<Is, Ts>...
{
};

/// A zip's item: one item of each zipped sequence, references kept, prvalues held by value.
/// A callback receives it spread, f(a, b), where it accepts that; materialized, it becomes a cc::tuple.
/// cc::tuple is only named here, so materializing needs <clean-core/container/tuple.hh>.
template <class... Ts>
struct seq_pack : seq_pack_storage<std::index_sequence_for<Ts...>, Ts...>
{
    using tuple_t = cc::tuple<std::remove_cvref_t<Ts>...>;

    // the I-th item with the pack's value category: a reference stays one, a held value moves out of an rvalue pack
    template <std::size_t I, class Self>
    [[nodiscard]] static constexpr decltype(auto) item(Self&& self)
    {
        using T = typename seq_type_at<I, Ts...>::type;
        using leaf_t = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, seq_pack_leaf<I, T> const,
                                          seq_pack_leaf<I, T>>;
        leaf_t& leaf = self;
        if constexpr (std::is_reference_v<T> || std::is_lvalue_reference_v<Self>)
            return (leaf.value);
        else
            return static_cast<T&&>(leaf.value);
    }

    // the tuple protocol, so a range-for over a zip can bind `auto&& [a, b]`
    template <std::size_t I>
    [[nodiscard]] constexpr decltype(auto) get() &
    {
        return seq_pack::item<I>(*this);
    }
    template <std::size_t I>
    [[nodiscard]] constexpr decltype(auto) get() const&
    {
        return seq_pack::item<I>(*this);
    }
    template <std::size_t I>
    [[nodiscard]] constexpr decltype(auto) get() &&
    {
        return seq_pack::item<I>(cc::move(*this));
    }

    [[nodiscard]] constexpr tuple_t to_tuple() const&
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        { return tuple_t(seq_pack::item<Is>(*this)...); }(std::index_sequence_for<Ts...>{});
    }
    [[nodiscard]] constexpr tuple_t to_tuple() &&
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        { return tuple_t(seq_pack::item<Is>(cc::move(*this))...); }(std::index_sequence_for<Ts...>{});
    }
};

template <class... Ts, std::size_t... Is, class... Args>
[[nodiscard]] constexpr seq_pack<Ts...> seq_make_pack_impl(std::index_sequence<Is...>, Args&&... args)
{
    return seq_pack<Ts...>{{seq_pack_leaf<Is, Ts>{static_cast<Ts&&>(args)}...}};
}

/// A pack of exactly Ts, each built from its argument.
template <class... Ts, class... Args>
[[nodiscard]] constexpr seq_pack<Ts...> seq_make_pack(Args&&... args)
{
    static_assert(sizeof...(Ts) == sizeof...(Args));
    return impl::seq_make_pack_impl<Ts...>(std::index_sequence_for<Ts...>{}, cc::forward<Args>(args)...);
}

template <class T>
inline constexpr bool seq_is_complete = requires { sizeof(T); };

template <class T>
inline constexpr bool seq_is_pack = false;
template <class... Ts>
inline constexpr bool seq_is_pack<seq_pack<Ts...>> = true;

/// The element type an item materializes as: the item decayed, or a pack's cc::tuple.
template <class ItemT>
struct seq_element_of
{
    using type = std::remove_cvref_t<ItemT>;
};
template <class ItemT>
    requires seq_is_pack<std::remove_cvref_t<ItemT>>
struct seq_element_of<ItemT>
{
    using type = typename std::remove_cvref_t<ItemT>::tuple_t;
};

/// An item as its element: a pack becomes its tuple, anything else is forwarded unchanged.
template <class X>
[[nodiscard]] constexpr decltype(auto) seq_as_element(X&& x)
{
    if constexpr (seq_is_pack<std::remove_cvref_t<X>>)
        return cc::forward<X>(x).to_tuple();
    else
        return cc::forward<X>(x);
}

template <class F, class X, std::size_t... Is>
constexpr decltype(auto) seq_invoke_spread(F& f, X&& x, std::index_sequence<Is...>)
{
    using pack_t = std::remove_cvref_t<X>;
    return cc::invoke(f, pack_t::template item<Is>(cc::forward<X>(x))...);
}
template <class F, class X, std::size_t... Is>
constexpr decltype(auto) seq_invoke_spread_idx(F& f, isize idx, X&& x, std::index_sequence<Is...>)
{
    using pack_t = std::remove_cvref_t<X>;
    return cc::invoke(f, idx, pack_t::template item<Is>(cc::forward<X>(x))...);
}

template <class F, class X, class Is>
inline constexpr bool seq_spread_invocable = false;
template <class F, class X, std::size_t... Is>
inline constexpr bool seq_spread_invocable<F, X, std::index_sequence<Is...>>
    = cc::is_invocable<F&, decltype(std::remove_cvref_t<X>::template item<Is>(std::declval<X>()))...>;
template <class F, class X, class Is>
inline constexpr bool seq_spread_invocable_idx = false;
template <class F, class X, std::size_t... Is>
inline constexpr bool seq_spread_invocable_idx<F, X, std::index_sequence<Is...>>
    = cc::is_invocable<F&, isize, decltype(std::remove_cvref_t<X>::template item<Is>(std::declval<X>()))...>;

/// How a callback takes an item, decided once at compile time.
enum class seq_call_form
{
    item,       // f(item)
    idx_item,   // f(idx, item)
    spread,     // f(a, b, ...) for a pack
    idx_spread, // f(idx, a, b, ...) for a pack
};

template <class F, class X>
[[nodiscard]] consteval seq_call_form seq_call_form_of()
{
    if constexpr (seq_is_pack<std::remove_cvref_t<X>>)
    {
        using is_t = std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<X>>>;
        if constexpr (seq_spread_invocable<F, X, is_t>)
            return seq_call_form::spread;
        else if constexpr (seq_spread_invocable_idx<F, X, is_t>)
            return seq_call_form::idx_spread;
        else if constexpr (cc::is_invocable<F&, X&&>)
            return seq_call_form::item;
        else
        {
            static_assert(cc::is_invocable<F&, isize, X&&>, "a callback over a zip takes (a, b, ...), (isize idx, a, "
                                                            "b, ...), (item) or (isize idx, item)");
            return seq_call_form::idx_item;
        }
    }
    else if constexpr (cc::is_invocable<F&, X&&>)
        return seq_call_form::item;
    else
    {
        static_assert(cc::is_invocable<F&, isize, X&&>, "the callback takes neither (item) nor (isize idx, item)");
        return seq_call_form::idx_item;
    }
}

template <class F, class X>
inline constexpr bool seq_call_takes_idx
    = seq_call_form_of<F, X>() == seq_call_form::idx_item || seq_call_form_of<F, X>() == seq_call_form::idx_spread;

/// Calls f with x in the form f takes; idx is the item's position and is advanced only when f takes it.
template <class F, class X>
constexpr decltype(auto) seq_call(F& f, isize& idx, X&& x)
{
    constexpr auto form = seq_call_form_of<F, X>();
    if constexpr (form == seq_call_form::item)
        return cc::invoke(f, cc::forward<X>(x));
    else if constexpr (form == seq_call_form::idx_item)
        return cc::invoke(f, idx++, cc::forward<X>(x));
    else
    {
        using is_t = std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<X>>>;
        if constexpr (form == seq_call_form::spread)
            return impl::seq_invoke_spread(f, cc::forward<X>(x), is_t{});
        else
            return impl::seq_invoke_spread_idx(f, idx++, cc::forward<X>(x), is_t{});
    }
}

template <class F, class X>
using seq_call_result_t = decltype(impl::seq_call(std::declval<F&>(), std::declval<isize&>(), std::declval<X>()));

/// A callback with its own position counter, for a sequence-layer reduction.
template <class F>
struct seq_indexed_call
{
    F& f;
    isize idx = 0;

    template <class X>
    constexpr decltype(auto) operator()(X&& x)
    {
        return impl::seq_call(f, idx, cc::forward<X>(x));
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
} // namespace cc::impl

template <class... Ts>
struct std::tuple_size<cc::impl::seq_pack<Ts...>> : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};
template <std::size_t I, class... Ts>
struct std::tuple_element<I, cc::impl::seq_pack<Ts...>>
{
    using type = typename cc::impl::seq_type_at<I, Ts...>::type;
};
