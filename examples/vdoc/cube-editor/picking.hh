#pragma once

#include "camera.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/ray.hh>

#include <limits> // no cc:: numeric limits yet

// The slab test below predates tg's geometry/query/, which now answers this pair; issue: chore-cube-editor-tg-ray-aabb-query

namespace cube_editor
{
/// The ray from the eye through a pixel's center, in world space.
[[nodiscard]] inline tg::ray3f pick_ray(orbit_camera const& cam, tg::angle_f vertical_fov, tg::pos2f pixel, tg::vec2i viewport)
{
    auto const size = tg::vec2f(float(viewport[0]), float(viewport[1]));
    // the near distance moves where a pick ray starts, never which way it points
    auto const vp = tg::view_projection_f::make(cam.view(), tg::perspective(vertical_fov, size[0] / size[1], 0.1f));
    auto const through = vp.unproject(tg::ndc_from_pixel(pixel + tg::vec2f(0.5f, 0.5f), size));
    return tg::ray3f(cam.eye(), through.dir);
}

/// Where `ray` enters `box`, or nothing if it misses.
/// The slab test: intersect the ray with each axis' pair of planes, and keep the overlap of the three intervals.
/// A zero direction component is handled by IEEE infinities rather than a branch — the resulting interval is
/// empty exactly when the origin lies outside that slab, which is the answer wanted.
[[nodiscard]] inline cc::optional<float> ray_aabb_entry(tg::ray3f const& ray, tg::aabb3f const& box)
{
    auto t_entry = 0.0f;
    auto t_exit = std::numeric_limits<float>::max();

    for (auto axis = 0; axis < 3; ++axis)
    {
        auto const inv_dir = 1.0f / ray.dir[axis];
        auto const t0 = (box.min[axis] - ray.origin[axis]) * inv_dir;
        auto const t1 = (box.max[axis] - ray.origin[axis]) * inv_dir;

        t_entry = cc::max(t_entry, cc::min(t0, t1));
        t_exit = cc::min(t_exit, cc::max(t0, t1));
    }

    if (t_exit < t_entry)
        return cc::nullopt;
    return t_entry;
}
} // namespace cube_editor
