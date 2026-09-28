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

/// A rect covering the pixels `[x0, x1) × [y0, y1)` of a `width` × `height` target.
[[nodiscard]] inline rect rect_at(int x0, int y0, int x1, int y1, int width, int height, float depth, tg::vec4f color)
{
    return rect{.rect = clip_rect(x0, y0, x1, y1, width, height), .depth = depth, .color = color};
}

/// The rects a test draws, uploaded once, and drawn a run at a time by their position in the list.
struct rect_batch
{
    sg::buffer<sg::test::sgl_shaders::rect_corner::per_vertex> corners;
    sg::buffer<rect> instances;

    /// The unit square's two triangles, counter-clockwise in clip space, then `rects` as instances.
    rect_batch(sg::context& ctx, cc::span<rect const> rects)
    {
        using corner = sg::test::sgl_shaders::rect_corner::per_vertex;
        corner const square[] = {
            {.at = tg::vec2f(0, 0)}, {.at = tg::vec2f(1, 0)}, {.at = tg::vec2f(1, 1)},
            {.at = tg::vec2f(0, 0)}, {.at = tg::vec2f(1, 1)}, {.at = tg::vec2f(0, 1)},
        };
        corners = ctx.persistent.create_buffer_from_data(square, sg::buffer_usage::vertex_buffer);
        instances = ctx.persistent.create_buffer_from_data(cc::vector<rect>::create_copy_of(rects),
                                                           sg::buffer_usage::vertex_buffer);
    }

    /// Binds both buffers and draws rects `[first, first + count)`.
    template <class Scope>
    void draw(Scope& scope, int first, int count = 1) const
    {
        scope.bind_vertex_buffers(
            sg::test::sgl_shaders::rect_corner::buffers{.per_vertex = corners, .per_instance = instances}.views());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = first, .size = count}});
    }
};
} // namespace sg_test
