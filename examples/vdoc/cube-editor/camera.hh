#pragma once

#include <typed-geometry/linalg/mat.hh>
#include <clean-core/common/utility.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/linalg/pos.hh>

namespace cube_editor
{
using namespace cc::primitive_defines;

/// The classic 3-vector cross product.
/// tg's `cross` is the wedge and returns a bivector, so the vector form is its Hodge dual.
[[nodiscard]] inline tg::vec3f cross3(tg::vec3f a, tg::vec3f b) { return tg::dual(tg::cross(a, b)); }

/// An orbit camera, which is the whole camera state this example has.
/// It is persisted in the vdoc file's workspace, so it survives a restart without ever becoming an edit.
struct orbit_camera
{
    tg::pos3f target = tg::pos3f::zero;
    float distance = 12.0f;
    tg::angle_f yaw = tg::angle_f::make_from_degree(35.0f);
    tg::angle_f pitch = tg::angle_f::make_from_degree(28.0f);

    [[nodiscard]] tg::pos3f eye() const
    {
        auto const cp = tg::cos(pitch);
        auto const dir = tg::vec3f(cp * tg::sin(yaw), tg::sin(pitch), cp * tg::cos(yaw));
        return target + dir * distance;
    }

    [[nodiscard]] tg::rigid_transform3f view() const { return tg::look_at(this->eye(), target); }

    /// Clamped just short of the poles, where the up vector and the view direction would become parallel.
    void orbit(tg::vec2f drag)
    {
        auto const limit = tg::angle_f::make_from_degree(89.0f);
        yaw = yaw + tg::angle_f::make_from_degree(drag[0] * 0.4f);
        pitch = cc::clamp(pitch + tg::angle_f::make_from_degree(drag[1] * 0.4f), -limit, limit);
    }

    void zoom(float ticks) { distance = cc::clamp(distance * tg::pow(1.1f, -ticks), 1.0f, 200.0f); }
};
} // namespace cube_editor
