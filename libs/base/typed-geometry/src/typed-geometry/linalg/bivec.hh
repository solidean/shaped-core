#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/linalg/fwd.hh>

#include <initializer_list>

/// Bivector in D dimensions — an oriented area element.
///
/// A bivector has C(D, 2) = D*(D-1)/2 components, one per pair of basis axes: 1 in 2D, 3 in 3D, 6 in 4D.
/// It is the result of the wedge product of two vectors, so in 3D cross(a, b) returns a bivec3 and dual()/undual() bridge to and from vec3 (linalg/cross.hh).
/// bivec is a linear space, so it supports addition and scalar multiplication.
///
/// Components live in the public array member `data`, reached through `data` or operator[], and the index runs over the C(D, 2) components rather than over D.
/// Default construction zero-initializes every component.
///
///     tg::bivec3f b;                                // {0, 0, 0}
///     auto const c = tg::cross(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0));  // bivec3
template <int D, class T>
struct tg::bivec
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
    /// all components zero.
    /// Runtime constant, not usable in constant expressions.
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

    // transformation
public:
    /// the image of this bivector under `t`, by the linear map's second exterior power.
    ///
    /// In 3D the dual — a normal — therefore picks up the cofactor matrix, which is why a normal must be a bivec and not a vec.
    /// The result is not renormalized; a non-uniform scaling changes its magnitude.
    ///
    /// The transform does the work, unconditionally: the exterior power needs cross.hh, which includes bivec,
    /// so it cannot be computed here.
    /// A transform that wants to answer for a bivec writes that answer in its own `transform`.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        return t.transform(*this);
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

namespace tg
{

template <int D, class T>
inline bivec<D, T> const bivec<D, T>::zero = bivec<D, T>{};

} // namespace tg
