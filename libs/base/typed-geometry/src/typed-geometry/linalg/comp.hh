#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>

#include <initializer_list>

namespace tg
{
/// Neutral component container: D values of T with no geometric semantics.
///
/// comp is the semantics-free building block. Unlike vec/pos it carries no notion of
/// direction or position; it is just "D components". This makes it the natural home for
/// raw, component-wise arithmetic — which it will gain in the future. For now it only
/// stores and indexes.
///
/// The raw storage is the public C array member `data`. Components are accessed through
/// `data` or operator[] — there are no .x/.y/.z members. Default construction
/// zero-initializes all components.
///
///     tg::comp3f c;                       // {0, 0, 0}
///     auto const s = tg::comp3f(1.0f);    // splat -> {1, 1, 1}
///     auto const v = tg::comp3f(1, 2, 3); // {1, 2, 3}
///     c[0] = 5;
template <int D, class T>
struct comp
{
    static_assert(D > 0, "comp requires a positive dimension");

    T data[D] = {};

    // construction
public:
    comp() = default;

    explicit constexpr comp(T value)
    {
        for (int i = 0; i < D; ++i)
            data[i] = value;
    }

    explicit constexpr comp(T x, T y)
        requires(D == 2)
      : data{x, y}
    {
    }
    explicit constexpr comp(T x, T y, T z)
        requires(D == 3)
      : data{x, y, z}
    {
    }
    explicit constexpr comp(T x, T y, T z, T w)
        requires(D == 4)
      : data{x, y, z, w}
    {
    }

    explicit comp(std::initializer_list<T> values)
    {
        CC_ASSERT(isize(values.size()) == D, "initializer list size must match dimension");
        int i = 0;
        for (auto const& v : values)
            data[i++] = v;
    }

    /// variadic construction; exactly D arguments are required.
    template <class... Ts>
    [[nodiscard]] static constexpr comp from_values(Ts... values)
        requires(sizeof...(Ts) == D)
    {
        comp r;
        int i = 0;
        ((r.data[i++] = T(values)), ...);
        return r;
    }

    // access
public:
    [[nodiscard]] constexpr T& operator[](int i)
    {
        CC_ASSERT(0 <= i && i < D, "component index out of range");
        return data[i];
    }
    [[nodiscard]] constexpr T const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < D, "component index out of range");
        return data[i];
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(comp const&, comp const&) = default;
};
} // namespace tg
