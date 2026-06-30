#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>

#include <initializer_list>

namespace tg
{
/// Bivector in D dimensions — an oriented area element.
///
/// A bivector has C(D, 2) = D*(D-1)/2 components (one per pair of basis axes): 1 in 2D, 3 in 3D,
/// 6 in 4D. It is the result of the wedge product of two vectors; in 3D, cross(a, b) returns a
/// bivec3 and dual()/undual() bridge to/from vec3 (see linalg/cross.hh). bivec is a linear space,
/// so it supports addition and scalar multiplication.
///
/// Storage is the public C array member `data`; components are accessed via `data` or operator[]
/// (the index runs over the C(D, 2) components, not over D). Default construction zero-initializes.
///
///     tg::bivec3f b;                                // {0, 0, 0}
///     auto const c = tg::cross(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0));  // bivec3
template <int D, class T>
struct bivec
{
    static_assert(D >= 2, "bivec requires at least 2 dimensions");

    /// number of independent bivector components, C(D, 2).
    static constexpr int num_components = D * (D - 1) / 2;

    T data[num_components] = {};

    // construction
public:
    bivec() = default;

    explicit constexpr bivec(T value)
    {
        for (int i = 0; i < num_components; ++i)
            data[i] = value;
    }

    explicit bivec(std::initializer_list<T> values)
    {
        CC_ASSERT(isize(values.size()) == num_components, "initializer list size must match component count");
        int i = 0;
        for (auto const& v : values)
            data[i++] = v;
    }

    /// variadic construction; exactly num_components arguments are required.
    template <class... Ts>
    [[nodiscard]] static constexpr bivec make_from_values(Ts... values)
        requires(sizeof...(Ts) == num_components)
    {
        bivec r;
        int i = 0;
        ((r.data[i++] = T(values)), ...);
        return r;
    }

    // special values
public:
    /// all components zero. Runtime constant, not usable in constant expressions.
    static bivec const zero;

    // access
public:
    [[nodiscard]] constexpr T& operator[](int i)
    {
        CC_ASSERT(0 <= i && i < num_components, "component index out of range");
        return data[i];
    }
    [[nodiscard]] constexpr T const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < num_components, "component index out of range");
        return data[i];
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(bivec const&, bivec const&) = default;

    // arithmetic
public:
    [[nodiscard]] friend constexpr bivec operator+(bivec a, bivec const& b)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] += b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr bivec operator-(bivec a, bivec const& b)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] -= b.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr bivec operator-(bivec a)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] = -a.data[i];
        return a;
    }
    [[nodiscard]] friend constexpr bivec operator*(bivec a, T s)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr bivec operator*(T s, bivec a)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr bivec operator/(bivec a, T s)
    {
        for (int i = 0; i < num_components; ++i)
            a.data[i] /= s;
        return a;
    }
};

template <int D, class T>
inline bivec<D, T> const bivec<D, T>::zero = bivec<D, T>{};

} // namespace tg
