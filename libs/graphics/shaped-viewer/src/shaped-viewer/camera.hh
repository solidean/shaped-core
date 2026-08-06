#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

namespace sv
{
/// GPU-side pinhole camera constants, matching the `Camera` struct in shaders/common.hlsli.
///
/// Each `float3` sits in its own 16-byte lane (the trailing pad scalars), which is the std140-ish cbuffer layout HLSL expects.
/// So this struct uploads straight into a uniform buffer.
/// `right_scaled` / `up_scaled` carry the aspect and field-of-view scaling pre-baked, so the raygen just forms `forward + right_scaled * ndc.x - up_scaled * ndc.y`.
struct camera_gpu
{
    tg::vec3f position;
    f32 _pad0 = 0;
    tg::vec3f forward;
    f32 _pad1 = 0;
    tg::vec3f right_scaled; // right * aspect * tan(fov_y / 2)
    f32 _pad2 = 0;
    tg::vec3f up_scaled; // true_up * tan(fov_y / 2)
    f32 _pad3 = 0;

    /// Bakes the pinhole basis from a camera; the aspect ratio is taken from `cam.projection.aspect_ratio`.
    [[nodiscard]] static camera_gpu from(camera const& cam);
};

/// A perspective projection: vertical field of view, aspect ratio (width / height), and near plane.
///
/// This is the only projection kind for now.
/// `aspect_ratio` is a property of the projection, not of the render target — set it from the target size before baking the GPU basis (the view renderer does this).
struct perspective_projection
{
    tg::angle_d vertical_fov = tg::angle_d::make_from_degree(60.0);
    f64 aspect_ratio = 1.0;
    f64 near_plane = 0.01;
};

/// A dev-friendly pinhole camera: a double-precision pose (position + orientation) plus a projection.
///
/// `orientation` is a unit quaternion mapping the base frame to the camera frame — it sends +x to right,
/// +y to up, +z to forward (left-handed, forward points into the scene, matching the raygen). Build one from
/// a look-at with `look_rotation` / `look_at`; the default frames the origin from `position`. The GPU basis is
/// baked by `camera_gpu::from`, taking the aspect ratio from `projection`.
struct camera
{
    tg::pos3d position = tg::pos3d(2.2, 1.8, -3.2);
    tg::quat_d orientation = look_rotation(position, tg::pos3d::zero);
    perspective_projection projection = {};

    /// A camera sitting at `eye` and looking at `target`, `up` fixing the roll; the projection stays default.
    [[nodiscard]] static camera looking_at(tg::pos3d eye, tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0))
    {
        return {.position = eye, .orientation = look_rotation(eye, target, up)};
    }

    /// A camera orbiting `target` at `distance`, looking inward; the projection stays default.
    ///
    /// `azimuth` orbits around +y (0 puts the eye on the -z side, looking along +z into the scene, matching the
    /// default pose); `elevation` lifts the eye above the horizon and must stay within (-90, 90) degrees so the
    /// view direction never aligns with +y.
    [[nodiscard]] static camera orbiting(tg::pos3d target, f64 distance, tg::angle_d azimuth, tg::angle_d elevation);

    /// The unit quaternion looking from `eye` toward `target`, with `up` fixing the roll (left-handed:
    /// forward = normalize(target - eye)). `up` must not be parallel to the view direction.
    [[nodiscard]] static tg::quat_d look_rotation(tg::pos3d eye, tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0));

    /// Aims the camera at `target` from its current `position`, keeping `up` as the roll reference.
    void look_at(tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0))
    {
        orientation = look_rotation(position, target, up);
    }
};
} // namespace sv
