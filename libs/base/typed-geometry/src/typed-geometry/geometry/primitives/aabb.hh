#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Axis-aligned bounding box in D dimensions.
///
/// Represents the solid box — the set of points {x : min[i] <= x[i] <= max[i] for all i}. It is a
/// full-dimensional, finite object (intrinsic_dim == ambient_dim == D). A well-formed aabb has
/// min <= max component-wise; this is not enforced at construction.
///
/// Default construction yields a degenerate box at the origin (min == max == origin).
///
///     tg::aabb3f b(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));   // the unit cube
template <int D, class T>
struct tg::aabb
{
    static_assert(D > 0, "aabb requires a positive dimension");

    pos<D, T> min;
    pos<D, T> max;

    // construction
public:
    aabb() = default;

    explicit constexpr aabb(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    // transformation
public:
    /// An aabb only survives the axis-aligned transforms — scaling and translation.
    ///
    /// Anything with a rotation in it is a compile error rather than a silently enlarged box;
    /// the type that would hold the answer is an oriented box, which tg does not have yet.
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
            auto const s = tg::scaling_translation_transform<D, T>(t);
            return aabb(min.transformed(s), max.transformed(s));
        }
        else if constexpr (requires { tg::signed_scaling_translation_transform<D, T>(t); })
        {
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
        else
            static_assert(false,
                          "tg: an aabb only survives scaling and translation. A rotated aabb is an oriented box, "
                          "and tg has no obb type yet.");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(aabb const&, aabb const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::aabb<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
