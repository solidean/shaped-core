#pragma once

#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/context/context.hh>

// Rects drawn by rects.sgl's `rect_vs`: one instance each, placed in clip space, at a depth and in a color.
// The fixed-function tests draw with them, one rect or a run of rects per draw.

namespace sg_test
{
using rect = sg::test::sgl_shaders::rect_corner::per_instance;
struct rect_batch;

/// A rect covering the pixels `[x0, x1) × [y0, y1)` of a `width` × `height` target.
[[nodiscard]] inline rect rect_at(int x0, int y0, int x1, int y1, int width, int height, float depth, tg::vec4f color)
{
    return rect{.rect = clip_rect(x0, y0, x1, y1, width, height), .depth = depth, .color = color};
}
} // namespace sg_test

/// The rects a test draws, uploaded once, and drawn a run at a time by their position in the list.
struct sg_test::rect_batch
{
    sg::buffer<sg::test::sgl_shaders::rect_corner::per_vertex> corners;
    sg::buffer<rect> instances;

    /// The unit square's two triangles, counter-clockwise in clip space, then `rects` as instances.
    rect_batch(sg::context& ctx, cc::span<rect const> rects) : rect_batch(ctx, rects, unit_square()) {}

    /// `rects` as instances over `points`, each a position in the unit square that a rect stretches over its pixels.
    rect_batch(sg::context& ctx, cc::span<rect const> rects, cc::span<tg::vec2f const> points)
    {
        auto at = cc::vector<sg::test::sgl_shaders::rect_corner::per_vertex>();
        for (auto const p : points)
            at.push_back({.at = p});
        vertex_count = int(points.size());
        corners = ctx.persistent.create_buffer_from_data(cc::move(at), sg::buffer_usage::vertex_buffer);
        instances = ctx.persistent.create_buffer_from_data(cc::vector<rect>::create_copy_of(rects),
                                                           sg::buffer_usage::vertex_buffer);
    }

    int vertex_count = 0;

    /// Binds both buffers and draws rects `[first, first + count)`, each over every point.
    template <class Scope>
    void draw(Scope& scope, int first, int count = 1) const
    {
        scope.bind_vertex_buffers(
            sg::test::sgl_shaders::rect_corner::buffers{.per_vertex = corners, .per_instance = instances}.views());
        scope.draw(
            {.vertex_range = {.offset = 0, .size = vertex_count}, .instance_range = {.offset = first, .size = count}});
    }

    /// Two counter-clockwise triangles covering the unit square.
    [[nodiscard]] static cc::span<tg::vec2f const> unit_square()
    {
        static tg::vec2f const square[] = {
            tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(1, 1), tg::vec2f(0, 0), tg::vec2f(1, 1), tg::vec2f(0, 1),
        };
        return square;
    }
};

namespace sg_test
{
/// The point of the unit square that a rect over a whole `width` × `height` target places at the pixel position `(x, y)`.
[[nodiscard]] inline tg::vec2f unit_at_pixel(float x, float y, int width, int height)
{
    return tg::vec2f(x / float(width), 1.0f - y / float(height));
}
} // namespace sg_test
