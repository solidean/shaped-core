#include "view_math.hh"

#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/compose.hh>

using namespace cc::primitive_defines;

tg::rigid_transform3f sxr::impl::camera_to_world_of(openxr_pose const& pose)
{
    auto const rotation = tg::rigid_transform3f::make_rotation(tg::quat_f(-pose.qx, -pose.qy, pose.qz, pose.qw));
    auto const translation = tg::rigid_transform3f::make_translation(tg::vec3f(pose.px, pose.py, -pose.pz));
    return tg::rigid_transform3f(tg::compose(translation, rotation));
}

tg::projective_transform3f sxr::impl::projection_of(openxr_fov const& fov, f32 near_distance, f32 far_distance)
{
    return tg::perspective_off_center_lh_zero_to_one(tg::tan(tg::angle_f::make_from_radians(fov.left)) * near_distance,
                                                     tg::tan(tg::angle_f::make_from_radians(fov.right)) * near_distance,
                                                     tg::tan(tg::angle_f::make_from_radians(fov.down)) * near_distance,
                                                     tg::tan(tg::angle_f::make_from_radians(fov.up)) * near_distance,
                                                     near_distance, far_distance);
}

tg::mat4f sxr::view::world_to_clip() const
{
    return projection.to_mat() * camera_to_world.to_mat().inverse();
}
