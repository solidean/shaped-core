#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Triangle (filled) with D-dimensional vertices.
///
/// Represents the solid triangle — the convex hull of its three vertices.
/// That is the set of points {a*pos0 + b*pos1 + c*pos2 : a,b,c >= 0, a+b+c == 1}.
/// It is a 2D surface patch (intrinsic_dim == 2) living in D-dimensional space (ambient_dim == D), and it is finite.
/// A triangle whose three vertices are collinear is degenerate, which is not enforced against at construction.
///
///     tg::triangle3f t(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
template <int D, class T>
struct tg::triangle
{
    static_assert(D > 0, "triangle requires a positive dimension");

    pos<D, T> pos0;
    pos<D, T> pos1;
    pos<D, T> pos2;

    // construction
public:
    triangle() = default;

    explicit constexpr triangle(pos<D, T> const& pos0, pos<D, T> const& pos1, pos<D, T> const& pos2)
      : pos0(pos0), pos1(pos1), pos2(pos2)
    {
    }

    // transformation
public:
    /// An affine map sends the convex hull of the vertices to the convex hull of their images.
    ///
    /// A projective map does too, but only while every vertex stays in front of the projection.
    /// That is NOT checked: a vertex behind it maps to its mirror image, and the result is a triangle over
    /// the wrong points rather than a diagnosed error.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return triangle(pos0.transformed(a), pos1.transformed(a), pos2.transformed(a));
        }
        else if constexpr (requires { tg::projective_transform<D, T>(t); })
        {
            auto const p = tg::projective_transform<D, T>(t);
            return triangle(pos0.transformed(p), pos1.transformed(p), pos2.transformed(p));
        }
        else
            static_assert(false, "tg: a triangle can be transformed by an affine or a projective map");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(triangle const&, triangle const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::triangle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
