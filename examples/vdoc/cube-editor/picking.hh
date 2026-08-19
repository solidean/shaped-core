#pragma once

#include "camera.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/ray.hh>

#include <limits> // no cc:: numeric limits yet

// TODO(typed-geometry): ray-aabb intersection belongs in tg, as the first occupant of geometry/query/.
// tg has aabb3f and ray3f but no query module at all — docs/plans/geometry-query-matrix.md is a proposal with no code —
// so the slab test lives here. The shape it should take there is
//   intersection_parameter(ray3<T>, aabb3<T>) -> cc::optional<hit_interval<T>>, plus an `intersects` predicate,
// which is roughly 50 lines of header and 90 of test. Picking is the reason it is worth having.

namespace cube_editor
{
/// The ray through a pixel, in world space.
/// Built from the camera basis rather than by inverting the view-projection, so it needs no matrix inverse.
[[nodiscard]] inline tg::ray3f pick_ray(orbit_camera const& cam, tg::angle_f vertical_fov, tg::pos2f pixel, tg::vec2i viewport)
{
    auto const eye = cam.eye();
    auto const f = tg::normalize(cam.target - eye);
    auto const r = tg::normalize(cross3(tg::vec3f(0, 1, 0), f));
    auto const u = cross3(f, r);

    auto const aspect = float(viewport[0]) / float(viewport[1]);
    auto const tan_half = tg::tan(vertical_fov / 2.0f);

    // Pixel centers, then to normalized device coordinates: x right, y UP, which is why y is flipped.
    auto const ndc_x = 2.0f * (pixel[0] + 0.5f) / float(viewport[0]) - 1.0f;
    auto const ndc_y = 1.0f - 2.0f * (pixel[1] + 0.5f) / float(viewport[1]);

    return tg::ray3f(eye, tg::normalize(f + r * (ndc_x * aspect * tan_half) + u * (ndc_y * tan_half)));
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
