#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// View transforms: world space into the camera's frame, as a rigid transform.
///
/// The frame is the one the projections in camera/projection.hh read: +x right, +y up, and +z forward for the
/// left-handed default, -z forward for `_rh`.
/// `up` only fixes the roll, so it need not be unit length or perpendicular to the view direction, but it must not be parallel to it.

namespace tg::impl
{
template <class T>
[[nodiscard]] vec<3, T> cross3(vec<3, T> const& a, vec<3, T> const& b)
{
    return tg::dual(tg::cross(a, b));
}

/// The rigid map taking world space into the frame whose axes, in world space, are `x`, `y`, `z` around `eye`.
template <class T>
[[nodiscard]] rigid_transform<3, T> make_view(pos<3, T> const& eye,
                                              vec<3, T> const& x,
                                              vec<3, T> const& y,
                                              vec<3, T> const& z)
{
    auto const camera_to_world = rigid_transform<3, T>::make_translation(eye - pos<3, T>::zero)
                                     .composed(rigid_transform<3, T>::make_rotation(quat<T>::make_from_basis(x, y, z)));
    return camera_to_world.inverse();
}
} // namespace tg::impl

namespace tg
{
/// The left-handed view looking along `forward`, which need not be unit length.
template <class T>
[[nodiscard]] rigid_transform<3, T> look_to(pos<3, T> const& eye,
                                            vec<3, T> const& forward,
                                            vec<3, T> const& up = vec<3, T>(0, 1, 0))
    requires(tg::traits::has_sqrt<T>)
{
    auto const f = forward.normalized();
    auto const r = impl::cross3(up, f).normalized();
    CC_ASSERT(!tg::traits::is_zero(r.length_sqr()), "look_to needs an up vector that is not parallel to the view "
                                                    "direction");
    return impl::make_view(eye, r, impl::cross3(f, r), f);
}

/// The left-handed view from `eye` toward `target`; the two must differ.
template <class T>
[[nodiscard]] rigid_transform<3, T> look_at(pos<3, T> const& eye,
                                            pos<3, T> const& target,
                                            vec<3, T> const& up = vec<3, T>(0, 1, 0))
    requires(tg::traits::has_sqrt<T>)
{
    return look_to(eye, target - eye, up);
}

/// The right-handed view looking along `forward`: the camera looks down its own -z.
template <class T>
[[nodiscard]] rigid_transform<3, T> look_to_rh(pos<3, T> const& eye,
                                               vec<3, T> const& forward,
                                               vec<3, T> const& up = vec<3, T>(0, 1, 0))
    requires(tg::traits::has_sqrt<T>)
{
    auto const f = forward.normalized();
    auto const r = impl::cross3(f, up).normalized();
    CC_ASSERT(!tg::traits::is_zero(r.length_sqr()), "look_to_rh needs an up vector that is not parallel to the view "
                                                    "direction");
    return impl::make_view(eye, r, impl::cross3(r, f), -f);
}

template <class T>
[[nodiscard]] rigid_transform<3, T> look_at_rh(pos<3, T> const& eye,
                                               pos<3, T> const& target,
                                               vec<3, T> const& up = vec<3, T>(0, 1, 0))
    requires(tg::traits::has_sqrt<T>)
{
    return look_to_rh(eye, target - eye, up);
}
} // namespace tg
