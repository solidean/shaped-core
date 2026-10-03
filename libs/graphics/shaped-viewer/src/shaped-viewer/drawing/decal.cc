#include <clean-core/common/assert.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/drawing/decal.hh>
#include <typed-geometry/linalg/bivec.hh>

namespace sv
{
namespace
{
[[nodiscard]] tg::vec3f cross(tg::vec3f a, tg::vec3f b)
{
    return tg::dual(tg::cross(a, b));
}

/// The row taking (x, y, z, 1) to one coordinate of the frame whose inverse row is `r`, around `at`.
[[nodiscard]] tg::vec4f row_of(tg::vec3f r, tg::pos3f at)
{
    return tg::vec4f(r[0], r[1], r[2], -tg::dot(r, at - tg::pos3f(0, 0, 0)));
}
} // namespace

shaders::tracer::decal_record impl::decal_record_of(decal_placement const& d, u32 first_shape)
{
    CC_ASSERT(d.depth > 0.0f, "a decal's projection must reach some depth");

    // A world point is at + X * x + Y * y + N * z, with z in [-1, 1] across the projection's reach.
    // The inverse of the matrix with columns X, Y, N has the cross products of the other two columns as its rows.
    auto const x = d.x_axis;
    auto const y = d.y_axis;
    auto const direction = cross(y, x).normalized();
    auto const n = direction * d.depth;
    auto const det = tg::dot(x, cross(y, n));
    CC_ASSERT(det != 0.0f, "a decal's axes must span a plane");

    return {.to_x = row_of(cross(y, n) / det, d.at),
            .to_y = row_of(cross(n, x) / det, d.at),
            .to_depth = row_of(cross(x, y) / det, d.at),
            .bounds = tg::vec4f(d.bounds.min[0], d.bounds.min[1], d.bounds.max[0], d.bounds.max[1]),
            .direction = direction,
            .first_shape = first_shape,
            .unit_length = tg::vec2f(x.length(), y.length()),
            .shape_count = d.record_count,
            .tint = d.tint};
}

shaders::tracer::decal_shape impl::decal_shape_of(sr::slug_instance const& r)
{
    return {.em_to_object = r.em_to_object,
            .em_bounds = r.em_bounds,
            .banding = r.banding,
            .origin = r.origin,
            .glyph = tg::vec<2, u32>(r.glyph_location, r.band_info),
            .color = r.color,
            .pad = {}};
}
} // namespace sv
