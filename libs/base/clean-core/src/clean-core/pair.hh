#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/utility.hh>

#include <utility>

/// Simple pair type holding two values of potentially different types
/// Aggregate type with no user-defined constructors
/// Supports structured bindings natively
template <class T, class U>
struct cc::pair
{
    using first_t = T;
    using second_t = U;

    [[nodiscard]] friend constexpr bool operator==(pair const&, pair const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(pair const&, pair const&) = default;

    T first;
    U second;

    template <std::size_t I, class P>
    [[nodiscard]] friend constexpr decltype(auto) get(P&& p) noexcept
        requires(std::is_same_v<std::remove_cvref_t<P>, pair> && I < 2)
    {
        if constexpr (I == 0)
            return cc::forward<P>(p).first;
        else
            return cc::forward<P>(p).second;
    }
};

namespace std
{
template <class T, class U>
struct tuple_size<cc::pair<T, U>> : std::integral_constant<std::size_t, 2>
{
};

template <std::size_t I, class T, class U>
struct tuple_element<I, cc::pair<T, U>>
{
    static_assert(I < 2);
    using type = std::conditional_t<I == 0, T, U>;
};
} // namespace std
