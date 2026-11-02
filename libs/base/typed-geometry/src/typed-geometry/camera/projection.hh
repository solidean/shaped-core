#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/error/optional.hh>
#include <typed-geometry/camera/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Projections: view space to clip space, for every convention in common use.
///
/// View space is the camera's frame: +x right, +y up, and +z forward for a left-handed function (`_lh`), -z forward for a right-handed one (`_rh`).
/// Clip space is normalized device coordinates after the divide: x and y in [-1, 1] with +y up, depth as the `tg::clip_depth` the name states.
/// libs/base/typed-geometry/docs/modules/camera.md is the convention, and why the unsuffixed default is left-handed reverse-Z.
///
/// Every function names its handedness and depth range; the unsuffixed ones are `_lh_reverse_z`.
/// A perspective function without a `far_distance` puts the far plane at infinity.
/// `near_distance` and `far_distance` are positive distances along the view axis, whichever way it points.
/// A perspective projection is a tg::projective_transform; an orthographic one is a per-axis scaling and offset, so an aabb stays an aabb under it.
/// An orthographic projection that flips depth carries negative_scaling, which is why its return type differs between conventions.

namespace tg::impl
{
enum class handedness : u8
{
    left,
    right,
};

/// z_clip = a * z + b against w = z, for a left-handed perspective; a right-handed one negates the z column.
template <class T>
struct perspective_depth_row
{
    T a;
    T b;
};

template <class T>
[[nodiscard]] constexpr perspective_depth_row<T> perspective_depth(clip_depth depth, T n, T f)
{
    CC_ASSERT(T(0) < n && n < f, "a perspective projection needs 0 < near_distance < far_distance");
    switch (depth)
    {
    case clip_depth::reverse_zero_to_one:
        return {.a = n / (n - f), .b = n * f / (f - n)};
    case clip_depth::zero_to_one:
        return {.a = f / (f - n), .b = -n * f / (f - n)};
    case clip_depth::minus_one_to_one:
        return {.a = (f + n) / (f - n), .b = T(-2) * f * n / (f - n)};
    }
    CC_UNREACHABLE("unknown clip_depth");
}

/// The limit of perspective_depth as the far distance goes to infinity.
template <class T>
[[nodiscard]] constexpr perspective_depth_row<T> perspective_depth_infinite(clip_depth depth, T n)
{
    CC_ASSERT(T(0) < n, "a perspective projection needs a positive near_distance");
    switch (depth)
    {
    case clip_depth::reverse_zero_to_one:
        return {.a = T(0), .b = n};
    case clip_depth::zero_to_one:
        return {.a = T(1), .b = -n};
    case clip_depth::minus_one_to_one:
        return {.a = T(1), .b = T(-2) * n};
    }
    CC_UNREACHABLE("unknown clip_depth");
}

/// `left` .. `top` are the view volume's extents on the near plane.
template <class T>
[[nodiscard]] constexpr projective_transform<3, T>
make_perspective(handedness hand, T left, T right, T bottom, T top, T n, perspective_depth_row<T> depth)
{
    CC_ASSERT(left < right && bottom < top, "a projection needs left < right and bottom < top");
    auto m = mat<4, 4, T>();
    m[0, 0] = T(2) * n / (right - left);
    m[1, 1] = T(2) * n / (top - bottom);
    // written for w = z, so the offset is subtracted: a point at x = right on the near plane has x_clip = w
    m[2, 0] = -(right + left) / (right - left);
    m[2, 1] = -(top + bottom) / (top - bottom);
    m[2, 2] = depth.a;
    m[3, 2] = depth.b;
    m[2, 3] = T(1);
    if (hand == handedness::right)
        for (auto r = 0; r < 4; ++r)
            m[2, r] = -m[2, r];
    return projective_transform<3, T>::make_from_mat(m);
}

template <class T>
[[nodiscard]] constexpr perspective_depth_row<T> perspective_depth_any(clip_depth depth, T n, cc::optional<T> const& f)
{
    return f.has_value() ? perspective_depth(depth, n, f.value()) : perspective_depth_infinite(depth, n);
}

template <class T>
[[nodiscard]] projective_transform<3, T> make_perspective_fov(handedness hand,
                                                              clip_depth depth,
                                                              angle<T> vertical_fov,
                                                              T aspect,
                                                              T n,
                                                              cc::optional<T> const& f)
    requires(tg::traits::has_trigonometry<T>)
{
    CC_ASSERT(aspect > T(0), "a perspective projection needs a positive aspect ratio");
    auto const half_height = n * (vertical_fov / T(2)).tan();
    auto const half_width = half_height * aspect;
    return make_perspective(hand, -half_width, half_width, -half_height, half_height, n,
                            perspective_depth_any(depth, n, f));
}

/// The scale and offset taking [n, f] along the view axis onto the depth range, for a left-handed view.
template <class T>
[[nodiscard]] constexpr vec<2, T> orthographic_depth(clip_depth depth, T n, T f)
{
    CC_ASSERT(n < f, "an orthographic projection needs near_distance < far_distance");
    switch (depth)
    {
    case clip_depth::reverse_zero_to_one:
        return vec<2, T>(T(-1) / (f - n), f / (f - n));
    case clip_depth::zero_to_one:
        return vec<2, T>(T(1) / (f - n), -n / (f - n));
    case clip_depth::minus_one_to_one:
        return vec<2, T>(T(2) / (f - n), -(f + n) / (f - n));
    }
    CC_UNREACHABLE("unknown clip_depth");
}

template <class ResultT, class T>
[[nodiscard]] constexpr ResultT
make_orthographic(handedness hand, clip_depth depth, T left, T right, T bottom, T top, T n, T f)
{
    CC_ASSERT(left < right && bottom < top, "a projection needs left < right and bottom < top");
    auto const z = orthographic_depth(depth, n, f);
    // a right-handed view looks down -z, so its depth axis runs the other way
    auto const z_scale = hand == handedness::right ? -z.data[0] : z.data[0];
    auto const scale = vec<3, T>(T(2) / (right - left), T(2) / (top - bottom), z_scale);
    auto const offset = vec<3, T>(-(right + left) / (right - left), -(top + bottom) / (top - bottom), z.data[1]);
    return ResultT::make_translation(offset).composed(ResultT::make_scaling(scale));
}

/// The orthographic projection of a view-space box: a right-handed view looks down -z, so its box's z extent is [-far, -near].
template <class ResultT, class T>
[[nodiscard]] constexpr ResultT make_orthographic_of_box(handedness hand, clip_depth depth, aabb<3, T> const& box)
{
    CC_ASSERT(!box.is_empty(), "the empty aabb has no orthographic projection");
    auto const n = hand == handedness::right ? -box.max.data[2] : box.min.data[2];
    auto const f = hand == handedness::right ? -box.min.data[2] : box.max.data[2];
    return make_orthographic<ResultT>(hand, depth, box.min.data[0], box.max.data[0], box.min.data[1], box.max.data[1],
                                      n, f);
}
} // namespace tg::impl

namespace tg
{
// =========================================================================================================
// The zoo: <kind>_<handedness>_<depth range>
// =========================================================================================================
//
// Each family has the default's overload set, so its names are, for every suffix below:
//   perspective_<suffix>(vertical_fov, aspect, near_distance[, far_distance])
//   perspective_off_center_<suffix>(left, right, bottom, top, near_distance[, far_distance])
//   orthographic_<suffix>(width, height, near_distance, far_distance)
//   orthographic_off_center_<suffix>(left, right, bottom, top, near_distance, far_distance)
//   orthographic_<suffix>(view_volume), the view-space box that becomes the clip volume
// with <suffix> one of lh_reverse_z, rh_reverse_z, lh_zero_to_one, rh_zero_to_one, lh_minus_one_to_one, rh_minus_one_to_one.
// An orthographic projection whose depth axis keeps its direction is a scaling_translation_transform.
// One that flips it is a signed_scaling_translation_transform.

#define TG_IMPL_PROJECTION_FAMILY(SUFFIX, HAND, DEPTH, ORTHO_T)                                                     \
    template <class T>                                                                                              \
    [[nodiscard]] projective_transform<3, T> perspective_##SUFFIX(angle<T> vertical_fov, T aspect, T near_distance) \
    {                                                                                                               \
        return impl::make_perspective_fov<T>(HAND, DEPTH, vertical_fov, aspect, near_distance, cc::optional<T>());  \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] projective_transform<3, T> perspective_##SUFFIX(angle<T> vertical_fov, T aspect, T near_distance, \
                                                                  T far_distance)                                   \
    {                                                                                                               \
        return impl::make_perspective_fov(HAND, DEPTH, vertical_fov, aspect, near_distance,                         \
                                          cc::optional<T>(far_distance));                                           \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] constexpr projective_transform<3, T> perspective_off_center_##SUFFIX(T left, T right, T bottom,   \
                                                                                       T top, T near_distance)      \
    {                                                                                                               \
        return impl::make_perspective(HAND, left, right, bottom, top, near_distance,                                \
                                      impl::perspective_depth_infinite(DEPTH, near_distance));                      \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] constexpr projective_transform<3, T> perspective_off_center_##SUFFIX(                             \
        T left, T right, T bottom, T top, T near_distance, T far_distance)                                          \
    {                                                                                                               \
        return impl::make_perspective(HAND, left, right, bottom, top, near_distance,                                \
                                      impl::perspective_depth(DEPTH, near_distance, far_distance));                 \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] constexpr ORTHO_T<3, T> orthographic_##SUFFIX(T width, T height, T near_distance, T far_distance) \
    {                                                                                                               \
        return impl::make_orthographic<ORTHO_T<3, T>>(HAND, DEPTH, -width / T(2), width / T(2), -height / T(2),     \
                                                      height / T(2), near_distance, far_distance);                  \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] constexpr ORTHO_T<3, T> orthographic_off_center_##SUFFIX(T left, T right, T bottom, T top,        \
                                                                           T near_distance, T far_distance)         \
    {                                                                                                               \
        return impl::make_orthographic<ORTHO_T<3, T>>(HAND, DEPTH, left, right, bottom, top, near_distance,         \
                                                      far_distance);                                                \
    }                                                                                                               \
    template <class T>                                                                                              \
    [[nodiscard]] constexpr ORTHO_T<3, T> orthographic_##SUFFIX(aabb<3, T> const& view_volume)                      \
    {                                                                                                               \
        return impl::make_orthographic_of_box<ORTHO_T<3, T>>(HAND, DEPTH, view_volume);                             \
    }

TG_IMPL_PROJECTION_FAMILY(lh_reverse_z,
                          impl::handedness::left,
                          clip_depth::reverse_zero_to_one,
                          signed_scaling_translation_transform)
TG_IMPL_PROJECTION_FAMILY(rh_reverse_z,
                          impl::handedness::right,
                          clip_depth::reverse_zero_to_one,
                          scaling_translation_transform)
TG_IMPL_PROJECTION_FAMILY(lh_zero_to_one, impl::handedness::left, clip_depth::zero_to_one, scaling_translation_transform)
TG_IMPL_PROJECTION_FAMILY(rh_zero_to_one,
                          impl::handedness::right,
                          clip_depth::zero_to_one,
                          signed_scaling_translation_transform)
TG_IMPL_PROJECTION_FAMILY(lh_minus_one_to_one,
                          impl::handedness::left,
                          clip_depth::minus_one_to_one,
                          scaling_translation_transform)
TG_IMPL_PROJECTION_FAMILY(rh_minus_one_to_one,
                          impl::handedness::right,
                          clip_depth::minus_one_to_one,
                          signed_scaling_translation_transform)

#undef TG_IMPL_PROJECTION_FAMILY

// =========================================================================================================
// The default: left-handed, reverse-Z
// =========================================================================================================

/// A perspective projection, without `far_distance` with its far plane at infinity: shaped-core's default.
/// `vertical_fov` is the full vertical opening angle, and `aspect` is width / height.
template <class T>
[[nodiscard]] projective_transform<3, T> perspective(angle<T> vertical_fov, T aspect, T near_distance)
{
    return perspective_lh_reverse_z(vertical_fov, aspect, near_distance);
}

template <class T>
[[nodiscard]] projective_transform<3, T> perspective(angle<T> vertical_fov, T aspect, T near_distance, T far_distance)
{
    return perspective_lh_reverse_z(vertical_fov, aspect, near_distance, far_distance);
}

/// A perspective projection whose view volume is asymmetric: `left` .. `top` are its extents on the near plane.
template <class T>
[[nodiscard]] constexpr projective_transform<3, T> perspective_off_center(T left, T right, T bottom, T top, T near_distance)
{
    return perspective_off_center_lh_reverse_z(left, right, bottom, top, near_distance);
}

template <class T>
[[nodiscard]] constexpr projective_transform<3, T> perspective_off_center(T left,
                                                                          T right,
                                                                          T bottom,
                                                                          T top,
                                                                          T near_distance,
                                                                          T far_distance)
{
    return perspective_off_center_lh_reverse_z(left, right, bottom, top, near_distance, far_distance);
}

/// An orthographic projection of a `width` x `height` view volume centered on the view axis.
template <class T>
[[nodiscard]] constexpr signed_scaling_translation_transform<3, T> orthographic(T width,
                                                                                T height,
                                                                                T near_distance,
                                                                                T far_distance)
{
    return orthographic_lh_reverse_z(width, height, near_distance, far_distance);
}

template <class T>
[[nodiscard]] constexpr signed_scaling_translation_transform<3, T> orthographic_off_center(T left,
                                                                                           T right,
                                                                                           T bottom,
                                                                                           T top,
                                                                                           T near_distance,
                                                                                           T far_distance)
{
    return orthographic_off_center_lh_reverse_z(left, right, bottom, top, near_distance, far_distance);
}

/// The orthographic projection whose view volume is `view_volume`, given in view space: its z extent is [near, far].
template <class T>
[[nodiscard]] constexpr signed_scaling_translation_transform<3, T> orthographic(aabb<3, T> const& view_volume)
{
    return orthographic_lh_reverse_z(view_volume);
}
} // namespace tg
