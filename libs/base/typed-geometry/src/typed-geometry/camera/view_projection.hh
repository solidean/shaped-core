#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/error/optional.hh>
#include <typed-geometry/camera/fwd.hh>
#include <typed-geometry/geometry/primitives/frustum.hh>
#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/primitives/ray.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// A view and a projection fused into one world-to-clip map, with its inverse and the depth convention it was built for.
///
/// `project` takes world space to normalized device coordinates (x, y in [-1, 1] with +y up, depth per `depth()`), and `unproject` takes it back.
/// The two are a pair, and the round trip is the contract:
///
///     unproject(project(p))      == p                         for every p in front of the camera
///     project(unproject(q))      == q                         for every q in the depth range
///     project(unproject(xy).at(t)).xy == xy                   for every t >= 0: the pick ray
///     unproject(*project(s))     == the part of s inside the view volume
///
/// A segment, a ray or a line projects to its visible part, clipped to the view volume in homogeneous clip space.
/// There a point at infinity is an ordinary point with w = 0, so a ray running off to an infinite far plane ends at its vanishing point on the far depth.
/// Clipping is what makes the answer a bounded segment whatever goes in; nothing visible gives none.
///
/// The convention is not read off the matrix: a zoo projection names its depth range, and the caller passes it on.
/// tests/camera/view-projection-test.cc pins every line of the contract above, for every convention.
template <class T>
struct tg::view_projection
{
    // construction
public:
    /// The identity: world space is clip space.
    view_projection() = default;

    /// Projection after view.
    /// Either may be any transform that widens to a projective one.
    template <class ViewT, class ProjectionT>
    [[nodiscard]] static view_projection make(ViewT const& view,
                                              ProjectionT const& projection,
                                              clip_depth depth = clip_depth::reverse_zero_to_one)
    {
        return view_projection::make_from_world_to_clip(
            projective_transform<3, T>(projection).composed(projective_transform<3, T>(view)), depth);
    }

    /// The map must be invertible.
    [[nodiscard]] static view_projection make_from_world_to_clip(projective_transform<3, T> const& world_to_clip,
                                                                 clip_depth depth = clip_depth::reverse_zero_to_one)
    {
        view_projection r;
        r._world_to_clip = world_to_clip;
        r._clip_to_world = world_to_clip.inverse();
        r._depth = depth;
        return r;
    }

    // readings
public:
    [[nodiscard]] constexpr projective_transform<3, T> const& world_to_clip() const { return _world_to_clip; }
    [[nodiscard]] constexpr projective_transform<3, T> const& clip_to_world() const { return _clip_to_world; }
    [[nodiscard]] constexpr clip_depth depth() const { return _depth; }

    /// The normalized depth the near plane maps to.
    [[nodiscard]] constexpr T near_depth() const
    {
        switch (_depth)
        {
        case clip_depth::reverse_zero_to_one:
            return T(1);
        case clip_depth::zero_to_one:
            return T(0);
        case clip_depth::minus_one_to_one:
            return T(-1);
        }
        CC_UNREACHABLE("unknown clip_depth");
    }

    /// The normalized depth the far plane maps to, which an infinite far plane reaches only at infinity.
    [[nodiscard]] constexpr T far_depth() const { return _depth == clip_depth::reverse_zero_to_one ? T(0) : T(1); }

    /// The view volume, read off the matrix with the factory matching `depth()`.
    [[nodiscard]] constexpr tg::frustum<3, T> frustum() const
        requires(tg::traits::has_sqrt<T>)
    {
        switch (_depth)
        {
        case clip_depth::reverse_zero_to_one:
            return tg::frustum<3, T>::make_from_view_projection(_world_to_clip);
        case clip_depth::zero_to_one:
            return tg::frustum<3, T>::make_from_view_projection_zero_to_one(_world_to_clip);
        case clip_depth::minus_one_to_one:
            return tg::frustum<3, T>::make_from_view_projection_minus_one_to_one(_world_to_clip);
        }
        CC_UNREACHABLE("unknown clip_depth");
    }

    // world to normalized device coordinates
public:
    /// `p` must be in front of the camera, where w > 0; it need not be inside the view volume.
    [[nodiscard]] constexpr pos<3, T> project(pos<3, T> const& p) const
    {
        auto const h = view_projection::apply(_world_to_clip, view_projection::homogeneous(p));
        CC_ASSERT(h.data[3] > T(0), "project needs a point in front of the camera");
        return view_projection::divide(h);
    }

    /// The part of `s` inside the view volume, in normalized device coordinates.
    [[nodiscard]] constexpr cc::optional<segment<3, T>> project(segment<3, T> const& s) const
    {
        auto const a = view_projection::apply(_world_to_clip, view_projection::homogeneous(s.pos0));
        auto const b = view_projection::apply(_world_to_clip, view_projection::homogeneous(s.pos1));
        return this->clip(a, b - a, interval{.has_lo = true, .lo = T(0), .has_hi = true, .hi = T(1)});
    }

    /// The part of `r` inside the view volume; with an infinite far plane it may end at its vanishing point.
    [[nodiscard]] constexpr cc::optional<segment<3, T>> project(ray<3, T> const& r) const
    {
        return this->clip(
            view_projection::apply(_world_to_clip, view_projection::homogeneous(r.origin)),
            view_projection::apply(_world_to_clip, vec<4, T>(r.dir.data[0], r.dir.data[1], r.dir.data[2], T(0))),
            interval{.has_lo = true, .lo = T(0), .has_hi = false, .hi = T(0)});
    }

    /// The part of `l` inside the view volume.
    [[nodiscard]] constexpr cc::optional<segment<3, T>> project(line<3, T> const& l) const
    {
        return this->clip(
            view_projection::apply(_world_to_clip, view_projection::homogeneous(l.origin)),
            view_projection::apply(_world_to_clip, vec<4, T>(l.dir.data[0], l.dir.data[1], l.dir.data[2], T(0))),
            interval{.has_lo = false, .lo = T(0), .has_hi = false, .hi = T(0)});
    }

    // normalized device coordinates to world
public:
    /// `ndc` must not be at an infinite far plane's depth, which no finite point has.
    [[nodiscard]] constexpr pos<3, T> unproject(pos<3, T> const& ndc) const
    {
        auto const h = view_projection::apply(_clip_to_world, view_projection::homogeneous(ndc));
        CC_ASSERT(!tg::traits::is_zero(h.data[3]), "unproject was given the depth of an infinite far plane");
        return view_projection::divide(h);
    }

    /// The pick ray through `ndc_xy`: it starts on the near plane, and every point on it projects back onto `ndc_xy`.
    /// Its direction is unit length; an orthographic projection gives every pixel the same one.
    [[nodiscard]] constexpr ray<3, T> unproject(pos<2, T> const& ndc_xy) const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const x = ndc_xy.data[0];
        auto const y = ndc_xy.data[1];
        auto const origin = this->unproject(pos<3, T>(x, y, this->near_depth()));
        // The direction comes from the far end, so its two points are as far apart as the view volume allows.
        // An infinite far plane's end is a point at infinity, w = 0, whose xyz is the direction itself.
        auto const far_end = view_projection::apply(_clip_to_world, vec<4, T>(x, y, this->far_depth(), T(1)));
        if (!tg::traits::is_zero(far_end.data[3]))
            return ray<3, T>(origin, (view_projection::divide(far_end) - origin).normalized());

        auto dir = vec<3, T>(far_end.data[0], far_end.data[1], far_end.data[2]);
        // a point at infinity has no sign, so point it the way that moves away from the camera
        if (view_projection::apply(_world_to_clip, vec<4, T>(dir.data[0], dir.data[1], dir.data[2], T(0))).data[3] < T(0))
            dir = -dir;
        return ray<3, T>(origin, dir.normalized());
    }

    /// The world segment `ndc` comes from; neither endpoint may be at an infinite far plane's depth.
    [[nodiscard]] constexpr segment<3, T> unproject(segment<3, T> const& ndc) const
    {
        return segment<3, T>(this->unproject(ndc.pos0), this->unproject(ndc.pos1));
    }

private:
    projective_transform<3, T> _world_to_clip;
    projective_transform<3, T> _clip_to_world;
    clip_depth _depth = clip_depth::reverse_zero_to_one;

    /// a parameter range: has_lo false means unbounded below, has_hi false unbounded above
    struct interval
    {
        bool has_lo;
        T lo;
        bool has_hi;
        T hi;
    };

    [[nodiscard]] static constexpr vec<4, T> homogeneous(pos<3, T> const& p)
    {
        return vec<4, T>(p.data[0], p.data[1], p.data[2], T(1));
    }

    [[nodiscard]] static constexpr vec<4, T> apply(projective_transform<3, T> const& t, vec<4, T> const& h)
    {
        return t.to_mat() * h;
    }

    [[nodiscard]] static constexpr pos<3, T> divide(vec<4, T> const& h)
    {
        return pos<3, T>(h.data[0] / h.data[3], h.data[1] / h.data[3], h.data[2] / h.data[3]);
    }

    /// The clip-space half-spaces a . h >= 0 bounding the view volume: x and y, then near and far depth.
    [[nodiscard]] constexpr cc::fixed_array<vec<4, T>, 6> clip_planes() const
    {
        auto const one = T(1);
        auto const zero = T(0);
        auto const depth = [&](vec<4, T> const& near_a, vec<4, T> const& far_a)
        {
            return cc::fixed_array<vec<4, T>, 6>{
                vec<4, T>(one, zero, zero, one),
                vec<4, T>(-one, zero, zero, one),
                vec<4, T>(zero, one, zero, one),
                vec<4, T>(zero, -one, zero, one),
                near_a,
                far_a,
            };
        };
        switch (_depth)
        {
        // z <= w and z >= 0
        case clip_depth::reverse_zero_to_one:
            return depth(vec<4, T>(zero, zero, -one, one), vec<4, T>(zero, zero, one, zero));
        // z >= 0 and z <= w
        case clip_depth::zero_to_one:
            return depth(vec<4, T>(zero, zero, one, zero), vec<4, T>(zero, zero, -one, one));
        // z >= -w and z <= w
        case clip_depth::minus_one_to_one:
            return depth(vec<4, T>(zero, zero, one, one), vec<4, T>(zero, zero, -one, one));
        }
        CC_UNREACHABLE("unknown clip_depth");
    }

    /// The part of { a + t b : t in range } inside the view volume, as an NDC segment.
    /// An end left unbounded by every plane is the point at infinity b (or -b), which lands at the vanishing point.
    [[nodiscard]] constexpr cc::optional<segment<3, T>> clip(vec<4, T> const& a, vec<4, T> const& b, interval range) const
    {
        for (auto const& plane : this->clip_planes())
        {
            auto const fa = dot(plane, a);
            auto const fb = dot(plane, b);
            if (tg::traits::is_zero(fb))
            {
                if (fa < T(0))
                    return {};
                continue;
            }
            auto const t = -fa / fb;
            if (fb > T(0))
            {
                if (!range.has_lo || t > range.lo)
                    range = {.has_lo = true, .lo = t, .has_hi = range.has_hi, .hi = range.hi};
            }
            else if (!range.has_hi || t < range.hi)
                range = {.has_lo = range.has_lo, .lo = range.lo, .has_hi = true, .hi = t};
        }
        if (range.has_lo && range.has_hi && range.lo > range.hi)
            return {};

        auto const end = [&](bool bounded, T t, T toward)
        {
            auto const h = bounded ? a + b * t : b * toward;
            CC_ASSERT(h.data[3] > T(0), "a clipped end lies behind the camera");
            return view_projection::divide(h);
        };
        return segment<3, T>(end(range.has_lo, range.lo, T(-1)), end(range.has_hi, range.hi, T(1)));
    }
};
