#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>

#include <initializer_list>

namespace tg
{
/// Neutral component container: D values of T with no geometric semantics.
///
/// comp is the semantics-free building block. Unlike vec/pos it carries no notion of
/// direction or position; it is just "D components". Precisely because it has no geometry, it
/// is the home of the **raw, fully component-wise arithmetic**: every operator acts element by
/// element, and a scalar operand broadcasts to all components. (vec/pos deliberately do NOT
/// offer Hadamard `*`/`/` or scalar broadcast — those only make sense on plain components.)
///
/// The raw storage is the public C array member `data`. Components are accessed through
/// `data` or operator[] — there are no .x/.y/.z members. Default construction
/// zero-initializes all components.
///
///     tg::comp3f c;                       // {0, 0, 0}
///     auto const s = tg::comp3f(1.0f);    // splat -> {1, 1, 1}
///     auto const v = tg::comp3f(1, 2, 3); // {1, 2, 3}
///     auto const h = v * v;               // {1, 4, 9} (component-wise)
///     auto const b = v + 10;              // {11, 12, 13} (scalar broadcast)
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
    [[nodiscard]] static constexpr comp make_from_values(Ts... values)
        requires(sizeof...(Ts) == D)
    {
        comp r;
        int i = 0;
        ((r.data[i++] = T(values)), ...);
        return r;
    }

    // special values
public:
    /// all components zero. Runtime constant, not usable in constant expressions.
    static comp const zero;

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

    // arithmetic — fully component-wise; a scalar operand broadcasts to all components
public:
    [[nodiscard]] friend constexpr comp operator-(comp a)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] = -a.data[i];
        return a;
    }

    [[nodiscard]] friend constexpr comp operator+(comp a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr comp operator-(comp a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr comp operator*(comp a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr comp operator/(comp a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= b.data[i];
        return a;
    }

    [[nodiscard]] friend constexpr comp operator+(comp a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += s;
        return a;
    }
    [[nodiscard]] friend constexpr comp operator+(T s, comp a) { return a + s; }
    [[nodiscard]] friend constexpr comp operator-(comp a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= s;
        return a;
    }
    [[nodiscard]] friend constexpr comp operator-(T s, comp const& a)
    {
        comp r;
        for (int i = 0; i < D; ++i)
            r.data[i] = s - a.data[i];
        return r;
    }
    [[nodiscard]] friend constexpr comp operator*(comp a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr comp operator*(T s, comp a) { return a * s; }
    [[nodiscard]] friend constexpr comp operator/(comp a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= s;
        return a;
    }
    [[nodiscard]] friend constexpr comp operator/(T s, comp const& a)
    {
        comp r;
        for (int i = 0; i < D; ++i)
            r.data[i] = s / a.data[i];
        return r;
    }

    friend constexpr comp& operator+=(comp& a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += b.data[i];
        return a;
    }
    friend constexpr comp& operator-=(comp& a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= b.data[i];
        return a;
    }
    friend constexpr comp& operator*=(comp& a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= b.data[i];
        return a;
    }
    friend constexpr comp& operator/=(comp& a, comp const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= b.data[i];
        return a;
    }
    friend constexpr comp& operator+=(comp& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += s;
        return a;
    }
    friend constexpr comp& operator-=(comp& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= s;
        return a;
    }
    friend constexpr comp& operator*=(comp& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= s;
        return a;
    }
    friend constexpr comp& operator/=(comp& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= s;
        return a;
    }
};

template <int D, class T>
inline comp<D, T> const comp<D, T>::zero = comp<D, T>{};

} // namespace tg
