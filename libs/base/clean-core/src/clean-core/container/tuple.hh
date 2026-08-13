#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <compare>
#include <type_traits>
#include <utility>

namespace cc::impl
{
/// One element of a tuple, tagged by its index so that duplicate element types stay distinct base classes.
/// An aggregate on purpose: every special member stays implicit, so triviality, move-only-ness and
/// copyability all propagate from T without a single hand-written operation.
template <std::size_t I, class T>
struct tuple_leaf
{
    [[no_unique_address]] T _value = {};

    [[nodiscard]] friend constexpr bool operator==(tuple_leaf const&, tuple_leaf const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(tuple_leaf const&, tuple_leaf const&) = default;
};

/// Recovers the element of a tuple by deducing T from the unique base carrying index I.
/// Four overloads rather than one forwarding one, because a forwarding reference cannot deduce through a
/// derived-to-base conversion and still report the argument's value category.
template <std::size_t I, class T>
[[nodiscard]] constexpr T& tuple_leaf_value(tuple_leaf<I, T>& leaf) noexcept
{
    return leaf._value;
}
template <std::size_t I, class T>
[[nodiscard]] constexpr T const& tuple_leaf_value(tuple_leaf<I, T> const& leaf) noexcept
{
    return leaf._value;
}
template <std::size_t I, class T>
[[nodiscard]] constexpr T&& tuple_leaf_value(tuple_leaf<I, T>&& leaf) noexcept
{
    return static_cast<T&&>(leaf._value);
}
template <std::size_t I, class T>
[[nodiscard]] constexpr T const&& tuple_leaf_value(tuple_leaf<I, T> const&& leaf) noexcept
{
    return static_cast<T const&&>(leaf._value);
}

template <class SeqT, class... Ts>
struct tuple_storage;

/// The leaves flattened into one class, so every element sits at a fixed depth and get<I> costs one instantiation.
template <std::size_t... Is, class... Ts>
struct tuple_storage<std::index_sequence<Is...>, Ts...> : tuple_leaf<Is, Ts>...
{
    tuple_storage() = default;

    /// Constrained so that it never outbids the implicit copy constructor when the base is copied.
    /// Parenthesized aggregate initialization of the leaves, so a narrowing element conversion stays legal.
    template <class... Us>
        requires(sizeof...(Us) == sizeof...(Ts) && (std::is_constructible_v<Ts, Us &&> && ...))
    constexpr explicit tuple_storage(Us&&... values) : tuple_leaf<Is, Ts>(cc::forward<Us>(values))...
    {
    }

    [[nodiscard]] friend constexpr bool operator==(tuple_storage const&, tuple_storage const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(tuple_storage const&, tuple_storage const&) = default;
};

/// The element type at index I, found by the same base deduction get<I> uses.
template <std::size_t I, class... Ts>
using tuple_element_t = std::remove_reference_t<decltype(cc::impl::tuple_leaf_value<I>(
    std::declval<tuple_storage<std::index_sequence_for<Ts...>, Ts...>&>()))>;
} // namespace cc::impl

/// Fixed-size heterogeneous collection of values of types Ts.
/// Access is INDEX-BASED only - get<I>(t) and structured bindings; there is deliberately no get<T>,
/// so duplicate element types are perfectly fine.
/// Trivially copyable and trivially destructible whenever every element is, and move-only elements make
/// the tuple move-only - all of that falls out of the implicit special members, none of which is written here.
/// Default construction VALUE-initializes every element, matching cc::pair.
/// Usage:
///   auto t = cc::tuple{1, 2.5f, cc::string("hi")};
///   auto const x = t.get<0>();         // 1, or get<0>(t)
///   auto& [a, b, c] = t;               // structured bindings
///   cc::apply([](int i, float f, auto&) { return i + f; }, t);
template <class... Ts>
struct cc::tuple : cc::impl::tuple_storage<std::index_sequence_for<Ts...>, Ts...>
{
    static_assert((std::is_object_v<Ts> && ...),
                  "tuple elements must be object types (no references, void or functions)");
    static_assert((!std::is_array_v<Ts> && ...), "tuple elements must not be raw arrays - use cc::fixed_array");

private:
    using storage_t = cc::impl::tuple_storage<std::index_sequence_for<Ts...>, Ts...>;

    // queries
public:
    /// Number of elements.
    static constexpr isize element_count = isize(sizeof...(Ts));

    /// The type of the element at index I.
    template <std::size_t I>
    using element_t = cc::impl::tuple_element_t<I, Ts...>;

    // construction
public:
    tuple() = default;

    /// Element-wise construction, implicit exactly when every element conversion is implicit.
    /// The last clause keeps a single-argument call from outbidding the implicit copy constructor.
    template <class... Us>
        requires(sizeof...(Us) == sizeof...(Ts) && sizeof...(Ts) > 0 //
                 && (std::is_constructible_v<Ts, Us &&> && ...)      //
                 && !(sizeof...(Us) == 1 && (std::is_same_v<std::remove_cvref_t<Us>, tuple> && ...)))
    explicit((!std::is_convertible_v<Us&&, Ts> || ...)) constexpr tuple(Us&&... values)
      : storage_t(cc::forward<Us>(values)...)
    {
    }

    // element access
public:
    /// Returns the element at index I, forwarding the tuple's value category.
    /// Uses deducing this (C++23), so the element comes back as T&, T const& or T&& to match.
    /// In a dependent context this needs the disambiguator: t.template get<0>().
    template <std::size_t I>
    [[nodiscard]] constexpr auto&& get(this auto&& self) noexcept
    {
        static_assert(I < sizeof...(Ts), "tuple index out of range");
        return cc::impl::tuple_leaf_value<I>(static_cast<decltype(self)&&>(self));
    }

    /// The same element, as a hidden friend, so generic get<I>(x) code reaches it through ADL.
    template <std::size_t I, class TupleT>
    [[nodiscard]] friend constexpr decltype(auto) get(TupleT&& t) noexcept
        requires(std::is_same_v<std::remove_cvref_t<TupleT>, tuple> && I < sizeof...(Ts))
    {
        return cc::impl::tuple_leaf_value<I>(static_cast<TupleT&&>(t));
    }

    /// Destroys the element at index I and constructs it in place from args.
    /// The only way to replace an element that is not assignable, and it needs no move.
    /// args must NOT alias that element, which is destroyed first.
    template <std::size_t I, class... Args>
    constexpr element_t<I>& emplace(Args&&... args)
    {
        static_assert(I < sizeof...(Ts), "tuple index out of range");
        static_assert(std::is_constructible_v<element_t<I>, Args...>, "element I must be constructible from Args...");

        using elem_t = element_t<I>;
        auto& elem = cc::impl::tuple_leaf_value<I>(*this);
        elem.~elem_t();
        return *new (cc::placement_new, &elem) elem_t(cc::forward<Args>(args)...);
    }

    // comparison
public:
    /// Element-wise equality and lexicographic ordering, both deleted when an element does not support them.
    [[nodiscard]] friend constexpr bool operator==(tuple const&, tuple const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(tuple const&, tuple const&) = default;

    // hashing
public:
    /// Order-dependent structural hash over the elements.
    [[nodiscard]] friend constexpr u64 hash(tuple const& t)
    {
        if constexpr (sizeof...(Ts) == 0)
            return 0;
        else
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) //
            { return cc::make_hash(cc::impl::tuple_leaf_value<Is>(t)...); }(std::index_sequence_for<Ts...>{});
    }
};

namespace cc
{
/// cc::tuple{1, 2.5f} is a cc::tuple<int, float>.
/// A deduction guide must be declared in the class template's own scope, hence the reopened namespace.
template <class... Ts>
tuple(Ts...) -> tuple<Ts...>;

/// Calls f with the tuple's elements as separate arguments, forwarding the tuple's value category.
/// Works for any tuple-like with std::tuple_size and a get<I>, so cc::pair and cc::fixed_array qualify too.
template <class F, class TupleT>
constexpr decltype(auto) apply(F&& f, TupleT&& t)
{
    using std::get; // the std overloads by ordinary lookup, our hidden friends by ADL

    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> decltype(auto)
    {
        return cc::invoke(cc::forward<F>(f), get<Is>(static_cast<TupleT&&>(t))...);
    }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<TupleT>>>{});
}
} // namespace cc

/// Specialization of std::tuple_size for tuple.
/// Mandatory rather than a nicety: with N distinct leaf bases the member-wise structured-binding path is
/// ill-formed, so the tuple-like protocol is the only one that can work.
template <class... Ts>
struct std::tuple_size<cc::tuple<Ts...>> : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};

/// Specialization of std::tuple_element for tuple.
template <std::size_t I, class... Ts>
struct std::tuple_element<I, cc::tuple<Ts...>>
{
    static_assert(I < sizeof...(Ts), "tuple index out of range");
    using type = cc::impl::tuple_element_t<I, Ts...>;
};
