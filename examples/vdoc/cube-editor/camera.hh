#pragma once

#include <typed-geometry/linalg/mat.hh>
#include <clean-core/common/utility.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/linalg/pos.hh>

// TODO(typed-geometry): perspective / look_at belong in tg's transform module.
// docs/modules/transform.md lists "camera / projection conventions" as planned, and until tg adopts a handedness
// and a depth range there is nothing to call — so the convention is pinned here instead: left-handed, z into [0, 1].
// The matrix layout is the part worth copying rather than re-deriving: tg::mat is COLUMN-major and subscripts m[col, row].
// libs/base/typed-geometry/tests/transform/projective-test.cc holds the same perspective matrix, for the same reason.

namespace cube_editor
{
using namespace cc::primitive_defines;

/// The classic 3-vector cross product.
/// tg's `cross` is the wedge and returns a bivector, so the vector form is its Hodge dual.
[[nodiscard]] inline tg::vec3f cross3(tg::vec3f a, tg::vec3f b) { return tg::dual(tg::cross(a, b)); }

/// A left-handed perspective projection with z mapped into [0, 1].
[[nodiscard]] inline tg::mat4f perspective(tg::angle_f vertical_fov, float aspect, float z_near, float z_far)
{
    auto const t = 1.0f / tg::tan(vertical_fov / 2.0f);

    auto m = tg::mat4f::zero;
    m[0, 0] = t / aspect;
    m[1, 1] = t;
    m[2, 2] = z_far / (z_far - z_near);
    m[3, 2] = -z_near * z_far / (z_far - z_near);
    m[2, 3] = 1.0f;
    return m;
}

/// A left-handed view matrix: world space into the camera's frame.
[[nodiscard]] inline tg::mat4f look_at(tg::pos3f eye, tg::pos3f target, tg::vec3f up)
{
    auto const f = tg::normalize(target - eye);
    auto const r = tg::normalize(cross3(up, f));
    auto const u = cross3(f, r);
    auto const e = eye - tg::pos3f::zero;

    auto m = tg::mat4f::identity;
    for (auto i = 0; i < 3; ++i)
    {
        m[i, 0] = r[i];
        m[i, 1] = u[i];
        m[i, 2] = f[i];
    }
    m[3, 0] = -tg::dot(r, e);
    m[3, 1] = -tg::dot(u, e);
    m[3, 2] = -tg::dot(f, e);
    return m;
}

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

    [[nodiscard]] tg::mat4f view() const { return look_at(this->eye(), target, tg::vec3f(0, 1, 0)); }

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
