#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Hyperplane in D dimensions, stored in Hesse normal form.
///
/// Represents the set of points {x : dot(normal, x) == dist} — the plane *itself*, not a side of it.
/// dist is the signed offset of the plane from the origin measured along normal, so the closest point to the origin is dist * normal when normal is unit-length.
/// It is a codimension-1 object (intrinsic_dim == D - 1) in D-dimensional space and is unbounded (is_finite == false).
/// In 2D this is a line, in 3D an ordinary plane.
///
/// normal is expected to be unit-length so that dist and dot(normal, x) read as true distances;
/// this is not enforced at construction.
///
/// Representation vs. interpretation: a future tg::halfspace will reuse this exact {normal, dist} encoding
/// but denote {x : dot(normal, x) <= dist} — one side of the plane rather than the plane.
///
///     tg::plane3f p(tg::vec3f(0, 0, 1), 5.0f);   // z == 5
template <int D, class T>
struct tg::plane
{
    static_assert(D > 0, "plane requires a positive dimension");

    vec<D, T> normal;
    T dist = {};

    // construction
public:
    plane() = default;

    explicit constexpr plane(vec<D, T> const& normal, T dist) : normal(normal), dist(dist) {}

    // transformation
public:
    /// A plane's normal is a covector, so it does NOT transform by the linear part.
    ///
    /// It picks up the cofactor matrix — determinant times the inverse transpose — which is exactly what keeps it orthogonal to the transformed plane.
    /// Transforming it as a displacement instead is the classic silent bug:
    /// under a non-uniform scaling the result stops being a normal.
    ///
    /// The cofactor form is also correct for the rigid and similarity cases,
    /// where it reduces to the rotation times a positive factor and the renormalization removes the factor.
    /// A projectivity preserves incidence, so a hyperplane stays a hyperplane there too;
    /// its homogeneous coordinates (normal, -dist) transform by the inverse transpose of the full matrix.
    template <class TransformT>
    [[nodiscard]] auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); } && tg::traits::has_sqrt<T>)
        {
            auto const a = tg::affine_transform<D, T>(t);
            auto const n = tg::normalize(a.linear_mat().cofactor() * normal);

            // dist is recovered from the image of `dist * normal`, a point known to be on the plane
            auto const on_plane = (normal * dist).transformed(a) + a.translation();
            return plane(n, tg::dot(n, on_plane));
        }
        else if constexpr (requires { tg::projective_transform<D, T>(t); } && tg::traits::has_sqrt<T>)
        {
            auto const p = tg::projective_transform<D, T>(t);

            vec<D + 1, T> h;
            for (int i = 0; i < D; ++i)
                h.data[i] = normal.data[i];
            h.data[D] = -dist;

            auto const image = p.to_mat().inverse().transposed() * h;

            vec<D, T> n;
            for (int i = 0; i < D; ++i)
                n.data[i] = image.data[i];

            // a degenerate image (the plane mapped to infinity) has len == 0 and yields non-finite values
            auto const len = n.length();
            return plane(n / len, -image.data[D] / len);
        }
        else
            static_assert(false, "tg: a plane can be transformed by an affine or a projective map over a scalar with "
                                 "sqrt");
    }

    // readings
public:
    /// the foot of the perpendicular from the origin.
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos<D, T>() + normal * dist; }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(plane const&, plane const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::plane<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};
