#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/hexahedron.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <type_traits>

/// Axis-aligned bounding box in D dimensions.
///
/// Represents the solid box — the set of points {x : min[i] <= x[i] <= max[i] for all i}, full-dimensional and finite.
///
/// **Default construction yields the empty box**, tg::aabb::empty: min at the scalar's highest value and max at its lowest.
/// For a float those are the infinities.
/// It is the empty set, and so the identity of union: accumulating bounds starts from it, and tg::aabb_of over nothing returns it.
/// min > max on any axis is empty, and is_empty() says so; `==` is no emptiness test, since many boxes are empty.
/// Every query verb answers for it once, in its dispatcher: it meets nothing, holds nothing but the empty set, and has no distance.
/// The measures and readings have no answer for it and refuse it, as does a rotating transform, which has no tg::box to give.
/// libs/base/typed-geometry/docs/modules/geometry.md has the decision.
///
/// tg::aabb_boundary shares the encoding and denotes the faces only; `.boundary()` and `.solid()` convert.
///
///     tg::aabb3f b(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));   // the unit cube
///     auto const box = tg::aabb3f::of(points, tri, p);         // the box around all of them
template <int D, class T>
struct tg::aabb
{
    using scalar_t = T;

    static_assert(D > 0, "aabb requires a positive dimension");

    pos<D, T> min = tg::impl::filled_pos<D>(tg::traits::highest<T>());
    pos<D, T> max = tg::impl::filled_pos<D>(tg::traits::lowest<T>());

    // construction
public:
    /// The empty box.
    aabb() = default;

    explicit constexpr aabb(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    /// The smallest box holding everything given: points, objects with bounds(), and ranges of either.
    /// Nothing at all, an empty range included, gives the empty box.
    template <class... Args>
    [[nodiscard]] static constexpr aabb of(Args const&... args);

    // special values
public:
    /// the empty box, which is also what default construction gives.
    static aabb const empty;

    // readings
public:
    [[nodiscard]] constexpr aabb_boundary<D, T> boundary() const { return aabb_boundary<D, T>(min, max); }

    /// whether the box holds no point: min > max on some axis.
    /// A box with min == max on an axis is flat or degenerate, not empty.
    [[nodiscard]] constexpr bool is_empty() const
    {
        for (int i = 0; i < D; ++i)
            if (max.data[i] < min.data[i])
                return true;
        return false;
    }

    // transformation
public:
    /// An aabb stays an aabb under the axis-aligned transforms — scaling and translation.
    /// Any other affine map makes it an oriented tg::box, never a silently enlarged aabb.
    ///
    /// With positive scale factors the corners keep their order, so the image is just the two images.
    /// Only a signed scaling can swap min and max along an axis, and only that case pays for the re-sort.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::scaling_translation_transform<D, T>(t); })
        {
            if (this->is_empty())
                return aabb();
            auto const s = tg::scaling_translation_transform<D, T>(t);
            return aabb(min.transformed(s), max.transformed(s));
        }
        else if constexpr (requires { tg::signed_scaling_translation_transform<D, T>(t); })
        {
            // a negative factor would turn the empty box's (+inf, -inf) into (-inf, +inf), all of space
            if (this->is_empty())
                return aabb();
            auto const s = tg::signed_scaling_translation_transform<D, T>(t);
            auto const a = min.transformed(s);
            auto const b = max.transformed(s);

            aabb result;
            for (int i = 0; i < D; ++i)
            {
                bool const ordered = a.data[i] < b.data[i];
                result.min.data[i] = ordered ? a.data[i] : b.data[i];
                result.max.data[i] = ordered ? b.data[i] : a.data[i];
            }
            return result;
        }
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            CC_ASSERT(!this->is_empty(), "the empty aabb has no tg::box image under a rotating transform");
            auto const a = tg::affine_transform<D, T>(t);
            auto const l = a.linear_mat();
            mat<D, D, T> h;
            for (int i = 0; i < D; ++i)
                h.cols[i] = l.cols[i] * ((max.data[i] - min.data[i]) / T(2));
            auto const center = min + (max - min) / T(2);
            return box<D, D, T>(center.transformed(a), h);
        }
        else
            static_assert(false, "tg: an aabb survives an affine map (as a tg::box at worst); its projective image "
                                 "has no tg type.");
    }

    // measures and readings
public:
    // every measure and reading below needs a non-empty box; bounds() alone returns the empty box as it is
    [[nodiscard]] constexpr pos<D, T> centroid() const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no centroid");
        return min + (max - min) / T(2);
    }
    [[nodiscard]] constexpr aabb bounds() const { return *this; }
    [[nodiscard]] constexpr pos<D, T> any_point() const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no point");
        return min;
    }
    /// in 2D the area; in 3D the area of the six faces.
    [[nodiscard]] constexpr T area() const
        requires(D == 2 || D == 3)
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no area");
        auto const s = max - min;
        if constexpr (D == 2)
            return s.data[0] * s.data[1];
        else
            return T(2) * (s.data[0] * s.data[1] + s.data[1] * s.data[2] + s.data[2] * s.data[0]);
    }
    [[nodiscard]] constexpr T perimeter() const
        requires(D == 2)
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no perimeter");
        auto const s = max - min;
        return T(2) * (s.data[0] + s.data[1]);
    }
    [[nodiscard]] constexpr T volume() const
        requires(D == 3)
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no volume");
        auto const s = max - min;
        return s.data[0] * s.data[1] * s.data[2];
    }
    /// corner i takes max on the axes whose bit is set in i.
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, (1 << D)> vertices() const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no vertices");
        cc::fixed_array<pos<D, T>, (1 << D)> r = {};
        for (int v = 0; v < (1 << D); ++v)
            for (int i = 0; i < D; ++i)
                r[v].data[i] = (v >> i) & 1 ? max.data[i] : min.data[i];
        return r;
    }
    /// every pair of corners that differ on exactly one axis.
    [[nodiscard]] constexpr cc::fixed_array<segment<D, T>, D * (1 << (D - 1))> edges() const
    {
        return tg::impl::corner_edges<D, D, T>(this->vertices());
    }
    /// the six faces, wound counterclockwise seen from outside; face 2a + s lies at max on axis a if s is 1, at min if 0.
    [[nodiscard]] constexpr cc::fixed_array<quad<3, T>, 6> faces() const
        requires(D == 3)
    {
        return tg::impl::corner_faces(this->vertices(), false);
    }

    // parameters
public:
    /// min + c * (max - min) per axis; the box is c in [0, 1]^D.
    [[nodiscard]] constexpr pos<D, T> at(comp<D, T> const& c) const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no parameterization");
        auto r = min;
        for (int i = 0; i < D; ++i)
            r.data[i] = min.data[i] + (max.data[i] - min.data[i]) * c.data[i];
        return r;
    }
    /// the inverse of at, unclamped: outside the box a coordinate leaves [0, 1].
    [[nodiscard]] constexpr comp<D, T> parameter_of(pos<D, T> const& p) const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no parameterization");
        comp<D, T> r;
        for (int i = 0; i < D; ++i)
            r.data[i] = (p.data[i] - min.data[i]) / (max.data[i] - min.data[i]);
        return r;
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb has no points to sample");
        auto r = min;
        for (int i = 0; i < D; ++i)
            r.data[i] = rng.uniform(min.data[i], max.data[i]);
        return r;
    }

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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(aabb const&, aabb const&) = default;
};

template <int D, class T>
inline tg::aabb<D, T> const tg::aabb<D, T>::empty = tg::aabb<D, T>();

template <int D, class T>
template <class... Args>
constexpr tg::aabb<D, T> tg::aabb<D, T>::of(Args const&... args)
{
    auto r = aabb();
    (tg::impl::grow_bounds(r, args), ...);
    return r;
}

namespace tg
{
/// The smallest box holding everything given: points, objects with bounds(), and ranges of either, all of one dimension and scalar.
/// An empty range gives the empty box; tg::aabb<D, T>::of names the box type, and takes no argument at all too.
template <class First, class... Rest>
[[nodiscard]] constexpr auto aabb_of(First const& first, Rest const&... rest)
{
    using box_t = typename tg::impl::bounds_aabb<First>::type;
    static_assert((std::is_same_v<box_t, typename tg::impl::bounds_aabb<Rest>::type> && ...),
                  "tg::aabb_of needs every argument in one dimension and scalar; tg::aabb<D, T>::of names the result");
    return box_t::of(first, rest...);
}
} // namespace tg

template <int D, class T>
struct tg::object_traits<tg::aabb<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

/// The boundary of a tg::aabb: the points of the box on at least one face, so intrinsic_dim is D - 1.
template <int D, class T>
struct tg::aabb_boundary
{
    using scalar_t = T;

    static_assert(D > 0, "aabb_boundary requires a positive dimension");

    pos<D, T> min = tg::impl::filled_pos<D>(tg::traits::highest<T>());
    pos<D, T> max = tg::impl::filled_pos<D>(tg::traits::lowest<T>());

    // construction
public:
    /// The boundary of the empty box, itself empty.
    aabb_boundary() = default;

    explicit constexpr aabb_boundary(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    // readings
public:
    [[nodiscard]] constexpr aabb<D, T> solid() const { return aabb<D, T>(min, max); }

    // transformation
public:
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
            return this->solid().transformed(t).boundary();
    }

    // readings
public:
    /// whether this is the boundary of the empty box, which holds no point either.
    [[nodiscard]] constexpr bool is_empty() const { return this->solid().is_empty(); }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<D, T> centroid() const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb_boundary has no centroid");
        return min + (max - min) / T(2);
    }
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return aabb<D, T>(min, max); }
    [[nodiscard]] constexpr pos<D, T> any_point() const
    {
        CC_ASSERT(!this->is_empty(), "the empty aabb_boundary has no point");
        return min;
    }
    /// the faces of a 2D box are its outline.
    [[nodiscard]] constexpr T length() const
        requires(D == 2)
    {
        return this->solid().perimeter();
    }
    [[nodiscard]] constexpr T area() const
        requires(D == 3)
    {
        return this->solid().area();
    }

    // sampling
public:
    /// a face chosen by its area, then a point on it: D + 1 draws, the face's own coordinate drawn and overwritten.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const s = max - min;
        T area[D] = {};
        auto total = T(0);
        for (int i = 0; i < D; ++i)
        {
            area[i] = T(1);
            for (int j = 0; j < D; ++j)
                if (j != i)
                    area[i] = area[i] * s.data[j];
            total = total + T(2) * area[i];
        }

        auto pick = rng.uniform(T(0), total);
        auto r = this->solid().sample_uniform(rng);
        for (int i = 0; i < D; ++i)
        {
            if (pick < area[i] || i == D - 1)
            {
                r.data[i] = min.data[i];
                return r;
            }
            pick = pick - area[i];
            if (pick < area[i])
            {
                r.data[i] = max.data[i];
                return r;
            }
            pick = pick - area[i];
        }
        return r;
    }

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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(aabb_boundary const&, aabb_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::aabb_boundary<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
