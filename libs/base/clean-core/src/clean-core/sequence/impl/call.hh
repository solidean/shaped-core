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

template <class ItemT>
struct seq_element_of;
template <class X>
[[nodiscard]] constexpr decltype(auto) seq_as_element(X&& x);

template <std::size_t... Is, class... Ts>
struct seq_pack_storage<std::index_sequence<Is...>, Ts...> : seq_pack_leaf<Is, Ts>...
{
};

/// A zip's item: one item of each zipped sequence, references kept, prvalues held by value.
/// A callback receives it spread, f(a, b), where it accepts that; materialized, it becomes a cc::tuple of values,
/// a nested pack (a window of a zip) a nested tuple, so nothing materialized references the source.
/// cc::tuple is only named here, so materializing needs <clean-core/container/tuple.hh>.
template <class... Ts>
struct seq_pack : seq_pack_storage<std::index_sequence_for<Ts...>, Ts...>
{
    using tuple_t = cc::tuple<typename seq_element_of<Ts>::type...>;

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
        { return tuple_t(impl::seq_as_element(seq_pack::item<Is>(*this))...); }(std::index_sequence_for<Ts...>{});
    }
    [[nodiscard]] constexpr tuple_t to_tuple() &&
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return tuple_t(impl::seq_as_element(seq_pack::item<Is>(cc::move(*this)))...);
        }(std::index_sequence_for<Ts...>{});
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

/// Whether an item refers to storage it does not hold: a reference, or a pack with a reference anywhere in it.
/// Such an item is what may dangle once its source steps or moves.
template <class T>
inline constexpr bool seq_holds_references = std::is_reference_v<T>;
template <class... Ts>
inline constexpr bool seq_holds_references<seq_pack<Ts...>> = (seq_holds_references<Ts> || ...);

/// An item with every reference in it replaced by a copy of what it refers to: U& is U, a pack holds values.
template <class T>
struct seq_owned
{
    using type = std::remove_cvref_t<T>;

    // the item as that copy; Move moves out of what a reference names, for a source that owns its elements
    template <bool Move, class X>
    [[nodiscard]] static constexpr type make(X&& x)
    {
        static_assert(!std::is_reference_v<T> || std::is_copy_constructible_v<type>
                          || (Move && std::is_move_constructible_v<type>),
                      "a window or chunk copies what an item references where the source may replace it at the "
                      "next step, and this element type cannot be copied: window an indexed or borrowed source");
        if constexpr (Move && std::is_reference_v<T>)
            return type(cc::move(x));
        else
            return type(cc::forward<X>(x));
    }
};
template <class... Ts>
struct seq_owned<seq_pack<Ts...>>
{
    using type = seq_pack<typename seq_owned<Ts>::type...>;

    template <bool Move, class X>
    [[nodiscard]] static constexpr type make(X&& x)
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return impl::seq_make_pack<typename seq_owned<Ts>::type...>(
                seq_owned<Ts>::template make<Move>(seq_pack<Ts...>::template item<Is>(cc::forward<X>(x)))...);
        }(std::index_sequence_for<Ts...>{});
    }
};

/// An item pulled from the source at from, as the same item of the source moved to to.
/// Every reference in it that points into the bytes of from moves to the same offset in to; see seq_rebase.
template <class T>
struct seq_rebased
{
    template <class S>
    [[nodiscard]] static constexpr T make(T&& x, S const&, S&)
    {
        return cc::move(x);
    }
};
template <class U>
struct seq_rebased<U&>
{
    template <class S>
    [[nodiscard]] static constexpr U& make(U& x, S const& from, S& to)
    {
        return *impl::seq_rebase(&x, from, to);
    }
};
template <class... Ts>
struct seq_rebased<seq_pack<Ts...>>
{
    template <class S>
    [[nodiscard]] static constexpr seq_pack<Ts...> make(seq_pack<Ts...>&& x, S const& from, S& to)
    {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return impl::seq_make_pack<Ts...>(
                seq_rebased<Ts>::make(seq_pack<Ts...>::template item<Is>(cc::move(x)), from, to)...);
        }(std::index_sequence_for<Ts...>{});
    }
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

/// How a callback takes an item, decided once at compile time by the first form it accepts.
/// Over a pack the order is spread, idx_spread, item, idx_item, element, idx_element, otherwise item, idx_item:
/// so f(isize i, auto w) over a zip of ints takes (a, b), never (idx, pack).
/// The element forms hand a pack over as its materialized cc::tuple, for a callback written against the element type.
/// A form is not checked for ambiguity: asking whether a generic lambda takes the other form would compile its body
/// with that form's arguments, which is a hard error rather than a no.
enum class seq_call_form
{
    item,        // f(item)
    idx_item,    // f(idx, item)
    spread,      // f(a, b, ...) for a pack
    idx_spread,  // f(idx, a, b, ...) for a pack
    element,     // f(tuple) for a pack
    idx_element, // f(idx, tuple) for a pack
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
        else if constexpr (cc::is_invocable<F&, isize, X&&>)
            return seq_call_form::idx_item;
        else
        {
            using tuple_t = typename std::remove_cvref_t<X>::tuple_t;
            if constexpr (cc::is_invocable<F&, tuple_t>)
                return seq_call_form::element;
            else
            {
                static_assert(cc::is_invocable<F&, isize, tuple_t>,
                              "a callback over a zip or window takes (a, b, ...), (isize idx, a, b, ...), (item), "
                              "(isize idx, item), or the materialized cc::tuple, with or without an index first");
                return seq_call_form::idx_element;
            }
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
    = seq_call_form_of<F, X>() == seq_call_form::idx_item || seq_call_form_of<F, X>() == seq_call_form::idx_spread
   || seq_call_form_of<F, X>() == seq_call_form::idx_element;

/// Calls f with x in the form f takes; idx is the item's position and is advanced only when f takes it.
template <class F, class X>
constexpr decltype(auto) seq_call(F& f, isize& idx, X&& x)
{
    constexpr auto form = seq_call_form_of<F, X>();
    if constexpr (form == seq_call_form::item)
        return cc::invoke(f, cc::forward<X>(x));
    else if constexpr (form == seq_call_form::idx_item)
        return cc::invoke(f, idx++, cc::forward<X>(x));
    else if constexpr (form == seq_call_form::element)
        return cc::invoke(f, impl::seq_as_element(cc::forward<X>(x)));
    else if constexpr (form == seq_call_form::idx_element)
        return cc::invoke(f, idx++, impl::seq_as_element(cc::forward<X>(x)));
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

/// The const view of an item a key or predicate callback receives, which must not consume it.
template <class ItemT>
using seq_view_t = std::remove_reference_t<ItemT> const&;

template <class X>
[[nodiscard]] constexpr std::remove_reference_t<X> const& seq_view(X&& x)
{
    return x;
}

/// f with its first argument bound, invocable exactly where f is, so seq_call still picks the form f takes after it.
template <class F, class A>
struct seq_bind_front
{
    F& f;
    A const& a;

    template <class... Xs>
        requires cc::is_invocable<F&, A const&, Xs...>
    constexpr decltype(auto) operator()(Xs&&... xs) const
    {
        return cc::invoke(f, a, cc::forward<Xs>(xs)...);
    }
};

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

/// The accumulator average() sums E in by default: i64 / u64 for integers, E for floating point, decltype(x + x) otherwise.
template <class E>
struct seq_default_average_acc
{
    using type = E;
};
template <class E>
    requires std::is_arithmetic_v<E>
struct seq_default_average_acc<E>
{
    using type = std::conditional_t<std::is_floating_point_v<E>, E, std::conditional_t<std::is_signed_v<E>, i64, u64>>;
};
template <class E>
    requires(!std::is_arithmetic_v<E>) && requires(E const& x) { x + x; }
struct seq_default_average_acc<E>
{
    using type = std::remove_cvref_t<decltype(std::declval<E const&>() + std::declval<E const&>())>;
};

/// How average() divides an accumulator AccT holding a sum of E by the count.
/// is_defined is false where it cannot, so average's assert can say why.
template <class E, class AccT>
struct seq_average
{
    static constexpr bool is_defined = false;
    using result_t = AccT;
    static constexpr AccT divide(AccT acc, isize) { return acc; }
};
/// An arithmetic accumulator: an integer one divides as f64, a floating-point one in its own type.
template <class E, class AccT>
    requires std::is_arithmetic_v<AccT>
struct seq_average<E, AccT>
{
    static constexpr bool is_defined = std::is_convertible_v<E, AccT>;
    using result_t = std::conditional_t<std::is_floating_point_v<AccT>, AccT, f64>;
    static constexpr result_t divide(AccT acc, isize n) { return result_t(acc) / result_t(n); }
};
/// Any other accumulator divides by its own scalar_t.
template <class E, class AccT>
    requires(!std::is_arithmetic_v<AccT>) && requires(AccT const& acc, isize n) { acc / typename AccT::scalar_t(n); }
struct seq_average<E, AccT>
{
    static constexpr bool is_defined = requires(AccT& acc, E const& x) { acc = acc + x; };
    using result_t = std::remove_cvref_t<decltype(std::declval<AccT const&>() / std::declval<typename AccT::scalar_t>())>;
    static constexpr result_t divide(AccT acc, isize n) { return acc / typename AccT::scalar_t(n); }
};

/// cc::sort and cc::sort_by, reached through argument-dependent lookup so this header need not include sort.hh.
/// The caller of a sorted materializer includes <clean-core/algorithm/sort.hh>, which is what makes the name visible.
template <class V, class... Args>
constexpr void seq_sort_adl(V& v, Args&&... args)
{
    sort(v, cc::forward<Args>(args)...);
}
template <class V, class KeyF>
constexpr void seq_sort_by_adl(V& v, KeyF&& key)
{
    sort_by(v, cc::forward<KeyF>(key));
}

/// join() and the cc::format formatter for sequences, defined in sequence/impl/format.hh, which string/format.hh includes.
template <class S>
struct seq_format_support;

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
