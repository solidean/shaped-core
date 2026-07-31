#include <shaped-viewer/camera.hh>
#include <typed-geometry/linalg/cross.hh>  // tg::cross + tg::dual
#include <typed-geometry/scalar/scalar.hh> // tg::sin_cos

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

camera_gpu camera_gpu::from(camera const& cam)
{
    auto const forward = cam.orientation * tg::vec3d(0, 0, 1);
    auto const right = cam.orientation * tg::vec3d(1, 0, 0);
    auto const true_up = cam.orientation * tg::vec3d(0, 1, 0);
    auto const t = (cam.projection.vertical_fov * 0.5).tan();

    auto const to_f = [](tg::vec3d const& v) { return tg::vec3f(f32(v[0]), f32(v[1]), f32(v[2])); };

    return {.position = tg::vec3f(f32(cam.position[0]), f32(cam.position[1]), f32(cam.position[2])),
            .forward = to_f(forward),
            .right_scaled = to_f(right * (cam.projection.aspect_ratio * t)),
            .up_scaled = to_f(true_up * t)};
}
} // namespace sv
