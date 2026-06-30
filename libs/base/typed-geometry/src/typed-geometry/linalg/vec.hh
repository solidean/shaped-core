#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <initializer_list>

namespace tg
{
/// Displacement / direction vector in D dimensions.
///
/// A vec is a free vector: it represents a difference between positions, a direction, or any
/// quantity that adds component-wise. vec + vec -> vec and scaling are well-defined; see pos
/// for the position type and its mixed pos/vec arithmetic.
///
/// The raw storage is the public C array member `data`. Components are accessed through `data`
/// or operator[] — there are no .x/.y/.z members. Default construction zero-initializes all
/// components. length()/normalized() are only available for scalar types whose scalar_traits
/// declares has_sqrt (see scalar/traits.hh).
///
///     tg::vec3f a;                          // {0, 0, 0}
///     auto const b = tg::vec3f(1, 2, 2);    // {1, 2, 2}
///     auto const c = a + b;                 // component-wise
///     auto const l = b.length();            // 3
///     auto const n = b.normalized();        // unit length
template <int D, class T>
struct vec
{
    static_assert(D > 0, "vec requires a positive dimension");

    T data[D] = {};

    // construction
public:
    vec() = default;

    explicit constexpr vec(T value)
    {
        for (int i = 0; i < D; ++i)
            data[i] = value;
    }

    explicit constexpr vec(T x, T y)
        requires(D == 2)
      : data{x, y}
    {
    }
    explicit constexpr vec(T x, T y, T z)
        requires(D == 3)
      : data{x, y, z}
    {
    }
    explicit constexpr vec(T x, T y, T z, T w)
        requires(D == 4)
      : data{x, y, z, w}
    {
    }

    explicit vec(std::initializer_list<T> values)
    {
        CC_ASSERT(isize(values.size()) == D, "initializer list size must match dimension");
        int i = 0;
        for (auto const& v : values)
            data[i++] = v;
    }

    /// variadic construction; exactly D arguments are required.
    template <class... Ts>
    [[nodiscard]] static constexpr vec from_values(Ts... values)
        requires(sizeof...(Ts) == D)
    {
        vec r;
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

    // measures
public:
    /// sum of squared components (no square root, available for all scalar types).
    [[nodiscard]] constexpr T length_sqr() const
    {
        T s = data[0] * data[0];
        for (int i = 1; i < D; ++i)
            s += data[i] * data[i];
        return s;
    }

    /// euclidean length; only for scalar types that support sqrt.
    [[nodiscard]] T length() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::sqrt(this->length_sqr());
    }

    /// unit vector in the same direction; only for scalar types that support sqrt.
    /// asserts the vector is non-zero.
    [[nodiscard]] vec normalized() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const l = this->length();
        CC_ASSERT(l != T{}, "cannot normalize a zero-length vector");
        return *this / l;
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(vec const&, vec const&) = default;

    // arithmetic
public:
    [[nodiscard]] friend constexpr vec operator+(vec a, vec const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr vec operator-(vec a, vec const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr vec operator-(vec a)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] = -a.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr vec operator*(vec a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr vec operator*(T s, vec a)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr vec operator/(vec a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= s;
        return a;
    }

    friend constexpr vec& operator+=(vec& a, vec const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] += b.data[i];
        return a;
    }
    friend constexpr vec& operator-=(vec& a, vec const& b)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] -= b.data[i];
        return a;
    }
    friend constexpr vec& operator*=(vec& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] *= s;
        return a;
    }
    friend constexpr vec& operator/=(vec& a, T s)
    {
        for (int i = 0; i < D; ++i)
            a.data[i] /= s;
        return a;
    }
};
} // namespace tg
