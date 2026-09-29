#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// Each drawn point writes (vertex index, instance index) into its pixel; a pixel no point reached keeps -1.
// So a first vertex, a base vertex or a first instance a backend dropped reads as a wrong number, not only a missing pixel.

namespace
{
constexpr int columns = 16;
constexpr int rows = 4;

/// One point per column and one row per instance, at pixel centres.
struct point_grid
{
    sg::buffer<shaders::indexed_point::per_vertex> vertices;
    sg::buffer<shaders::indexed_point::per_instance> instances;

    explicit point_grid(sg::context& ctx)
    {
        auto v = cc::vector<shaders::indexed_point::per_vertex>();
        for (auto c = 0; c < columns; ++c)
            v.push_back({.column = -1.0f + (2.0f * float(c) + 1.0f) / float(columns)});
        auto i = cc::vector<shaders::indexed_point::per_instance>();
        for (auto r = 0; r < rows; ++r)
            i.push_back({.row = 1.0f - (2.0f * float(r) + 1.0f) / float(rows)});
        vertices = ctx.persistent.create_buffer_from_data(cc::move(v), sg::buffer_usage::vertex_buffer);
        instances = ctx.persistent.create_buffer_from_data(cc::move(i), sg::buffer_usage::vertex_buffer);
    }

    void bind(sg::rendering_scope& scope) const
    {
        scope.bind_vertex_buffers(
            shaders::indexed_point::buffers{.per_vertex = vertices, .per_instance = instances}.views());
    }
};

sg_test::offscreen grid_target()
{
    return {.width = columns,
            .height = rows,
            .colors = {sg::pixel_format::rgba32_float},
            .target_set = shaders::index_target::name,
            .clear_color = tg::vec4f(-1, -1, -1, -1)};
}

/// What pixel (c, r) holds: the indices a point drawn there wrote, or -1 for none.
tg::vec2f indices_at(sg_test::offscreen_pixels const& pixels, int c, int r)
{
    auto const v = pixels[0].rgba_float(c, r);
    return tg::vec2f(v[0], v[1]);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a draw's first vertex, first instance and instance count reach @vertex_index and "
                     "@instance_index",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::draws.indexed_points);
    auto const grid = point_grid(*ctx);

    // Vertices 3 to 6 of instances 1 and 2, then vertex 12 alone of instance 3.
    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx, grid_target(),
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            grid.bind(scope);
            scope.draw({.vertex_range = {.offset = 3, .size = 4}, .instance_range = {.offset = 1, .size = 2}});
            scope.draw({.vertex_range = {.offset = 12, .size = 1}, .instance_range = {.offset = 3, .size = 1}});
        });

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < columns; ++c)
        {
            auto const first = c >= 3 && c < 7 && r >= 1 && r < 3;
            auto const second = c == 12 && r == 3;
            auto const expected = first || second ? tg::vec2f(float(c), float(r)) : tg::vec2f(-1, -1);
            CHECK(indices_at(pixels, c, r) == expected).context(cc::format("pixel ({}, {})", c, r));
        }
}

ASYNC_INVOCABLE_TEST("sg - an indexed draw adds its vertex offset to 16- and 32-bit indices, and @vertex_index "
                     "includes it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::draws.indexed_points);
    auto const grid = point_grid(*ctx);

    // Row 0: 16-bit indices 0, 2, 4 from the second index pair on, offset by 5, reach columns 5, 7 and 9.
    // (A 16-bit draw starts at an even index, since sg refuses an odd one on every backend.)
    // Row 1: 32-bit indices 1, 3, 6 from an odd first index, offset by 2, reach columns 3, 5 and 8.
    // Index 15 is a decoy either draw reaches only by ignoring its first index.
    u16 const short_indices[] = {15, 15, 0, 2, 4};
    u32 const long_indices[] = {15, 1, 3, 6};
    auto const shorts = ctx->persistent.create_buffer_from_data(short_indices, sg::buffer_usage::index_buffer);
    auto const longs = ctx->persistent.create_buffer_from_data(long_indices, sg::buffer_usage::index_buffer);

    auto const pixels
        = co_await sg_test::draw_offscreen(*ctx, grid_target(),
                                           [&](sg::rendering_scope& scope)
                                           {
                                               scope.bind_pipeline(*pipeline);
                                               grid.bind(scope);
                                               scope.bind_index_buffer(shorts.as_index_buffer());
                                               scope.draw_indexed({.index_range = {.offset = 2, .size = 3},
                                                                   .instance_range = {.offset = 0, .size = 1},
                                                                   .vertex_offset = 5});
                                               scope.bind_index_buffer(longs.as_index_buffer());
                                               scope.draw_indexed({.index_range = {.offset = 1, .size = 3},
                                                                   .instance_range = {.offset = 1, .size = 1},
                                                                   .vertex_offset = 2});
                                           });

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < columns; ++c)
        {
            auto const drawn = (r == 0 && (c == 5 || c == 7 || c == 9)) || (r == 1 && (c == 3 || c == 5 || c == 8));
            auto const expected = drawn ? tg::vec2f(float(c), float(r)) : tg::vec2f(-1, -1);
            CHECK(indices_at(pixels, c, r) == expected).context(cc::format("pixel ({}, {})", c, r));
        }
}

ASYNC_INVOCABLE_TEST("sg - vertex buffers bound from a first slot other than 0 feed the slots they name",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::draws.indexed_points);
    auto const grid = point_grid(*ctx);
    // A second instance buffer whose rows run bottom to top, bound alone at slot 1 between two draws.
    auto reversed = cc::vector<shaders::indexed_point::per_instance>();
    for (auto r = rows - 1; r >= 0; --r)
        reversed.push_back({.row = 1.0f - (2.0f * float(r) + 1.0f) / float(rows)});
    auto const reversed_rows
        = ctx->persistent.create_buffer_from_data(cc::move(reversed), sg::buffer_usage::vertex_buffer);

    auto const pixels
        = co_await sg_test::draw_offscreen(*ctx, grid_target(),
                                           [&](sg::rendering_scope& scope)
                                           {
                                               scope.bind_pipeline(*pipeline);
                                               grid.bind(scope);
                                               // instance 0 in row 0
                                               scope.draw({.vertex_range = {.offset = 0, .size = 1}});
                                               scope.bind_vertex_buffers({reversed_rows.as_vertex_buffer()}, 1);
                                               // instance 0 now in the last row, and the vertices unchanged
                                               scope.draw({.vertex_range = {.offset = 1, .size = 1}});
                                           });

    CHECK(indices_at(pixels, 0, 0) == tg::vec2f(0, 0));
    CHECK(indices_at(pixels, 1, rows - 1) == tg::vec2f(1, 0));
    CHECK(indices_at(pixels, 1, 0) == tg::vec2f(-1, -1));
}

ASYNC_INVOCABLE_TEST("sg - inline constants set between two draws reach each its own", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::draws.tagged_points);
    auto const grid = point_grid(*ctx);

    // Column 2 drawn tagged 7, then column 5 tagged 9, in one scope.
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, grid_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             grid.bind(scope);
                                                             scope.set_inline_constants(shaders::draw_tag{.tag = 7.0f});
                                                             scope.draw({.vertex_range = {.offset = 2, .size = 1}});
                                                             scope.set_inline_constants(shaders::draw_tag{.tag = 9.0f});
                                                             scope.draw({.vertex_range = {.offset = 5, .size = 1}});
                                                         });

    CHECK(pixels[0].rgba_float(2, 0) == tg::vec4f(2, 0, 7, 1));
    CHECK(pixels[0].rgba_float(5, 0) == tg::vec4f(5, 0, 9, 1));
}
