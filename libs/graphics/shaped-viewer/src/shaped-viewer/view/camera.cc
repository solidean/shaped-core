#include <shaped-viewer/view/camera.hh>
#include <typed-geometry/linalg/cross.hh>   // tg::cross + tg::dual
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot
#include <typed-geometry/scalar/scalar.hh>  // tg::sin_cos

namespace sv
{
camera camera::orbiting(tg::pos3d target, f64 distance, tg::angle_d azimuth, tg::angle_d elevation)
{
    auto const [sa, ca] = tg::sin_cos(azimuth);
    auto const [se, ce] = tg::sin_cos(elevation);
    auto const offset = tg::vec3d(distance * ce * sa, distance * se, -distance * ce * ca);
    return looking_at(target + offset, target);
}

tg::quat_d camera::look_rotation(tg::pos3d eye, tg::pos3d target, tg::vec3d up)
{
    auto const forward = (target - eye).normalized();
    auto const right = tg::dual(tg::cross(up, forward)).normalized();
    auto const true_up = tg::dual(tg::cross(forward, right));
    return tg::quat_d::make_from_basis(right, true_up, forward);
}

camera_basis camera::basis() const
{
    return {.right = orientation * tg::vec3d(1, 0, 0),
            .up = orientation * tg::vec3d(0, 1, 0),
            .forward = orientation * tg::vec3d(0, 0, 1)};
}

camera_gpu camera_gpu::from(camera const& cam)
{
    auto const b = cam.basis();
    auto const t = (cam.projection.vertical_fov * 0.5).tan();

    auto const to_f = [](tg::vec3d const& v) { return tg::vec3f(f32(v[0]), f32(v[1]), f32(v[2])); };

    return {.position = tg::vec3f(f32(cam.position[0]), f32(cam.position[1]), f32(cam.position[2])),
            .forward = to_f(b.forward),
            .right_scaled = to_f(b.right * (cam.projection.aspect_ratio * t)),
            .up_scaled = to_f(b.up * t)};
}

camera_matrices matrices_of(camera_gpu const& cam, f32 near_plane)
{
    auto const tan_x = cam.right_scaled.length();
    auto const tan_y = cam.up_scaled.length();
    auto const forward_length = cam.forward.length();
    if (tan_x <= 0 || tan_y <= 0 || forward_length <= 0)
        return {};

    // The view axes, which `camera_project` reads the same three dot products against.
    auto const r = cam.right_scaled / tan_x;
    auto const u = cam.up_scaled / tan_y;
    auto const f = cam.forward / forward_length;

    // A view basis is a rotation, so its inverse is its transpose, and the translation is that applied to -position.
    auto world_to_view = tg::mat4f::identity;
    for (auto axis = 0; axis < 3; ++axis)
    {
        world_to_view[axis, 0] = r[axis];
        world_to_view[axis, 1] = u[axis];
        world_to_view[axis, 2] = f[axis];
    }
    world_to_view[3, 0] = -tg::dot(cam.position, r);
    world_to_view[3, 1] = -tg::dot(cam.position, u);
    world_to_view[3, 2] = -tg::dot(cam.position, f);

    // Left-handed with an infinite far plane, and `w` is the view depth — so clip-space depth is `z / w`, which is
    // what a denoiser decomposing this projection expects.
    // y is up here, where `camera_project`'s pixel space is y down; the flip belongs to that function rather than to
    // the projection.
    auto view_to_clip = tg::mat4f::zero;
    view_to_clip[0, 0] = 1.0f / tan_x;
    view_to_clip[1, 1] = 1.0f / tan_y;
    view_to_clip[2, 2] = 1.0f;
    view_to_clip[3, 2] = -near_plane;
    view_to_clip[2, 3] = 1.0f;

    return {.world_to_view = world_to_view, .view_to_clip = view_to_clip};
}
} // namespace sv
