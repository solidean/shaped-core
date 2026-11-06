#pragma once

#include <shaped-xr/xr.hh>

namespace sxr::impl
{
/// An OpenXR pose as its plain numbers: a unit quaternion (x, y, z, w) and a position, right-handed with -z forward.
struct openxr_pose
{
    f32 qx = 0, qy = 0, qz = 0, qw = 1;
    f32 px = 0, py = 0, pz = 0;
};

/// An OpenXR field of view: the four angles of the frustum's sides from the view direction, in radians; left and down
/// are negative for a frustum around it.
struct openxr_fov
{
    f32 left = 0, right = 0, up = 0, down = 0;
};

/// The pose in tg's left-handed frame: the position's z and the quaternion's x and y negated, which mirrors z.
[[nodiscard]] tg::rigid_transform3f camera_to_world_of(openxr_pose const& pose);

/// The eye's projection: left-handed, depth 0 at `near_distance` and 1 at `far_distance`, from the four angles' tangents.
[[nodiscard]] tg::projective_transform3f projection_of(openxr_fov const& fov, f32 near_distance, f32 far_distance);
} // namespace sxr::impl
