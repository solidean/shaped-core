#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// What SGL states for every target and only a GPU shows a backend doing: pixel_semantics.sgl's stages, drawn and read.

namespace
{
using corner = shaders::tagged_corner;

/// The two triangles of the clip-space rect `r` (left, bottom, right, top), counter-clockwise, every vertex at `depth`.
/// The first triangle is the lower-right one and the second the upper-left, and each takes its tags in order.
void push_rect(cc::vector<corner>& out, tg::vec4f r, float depth, cc::span<int const> tags)
{
    auto const at
        = [&](float x, float y, int tag) { out.push_back({.position = tg::vec2f(x, y), .tag = tag, .depth = depth}); };
    at(r[0], r[1], tags[0]);
    at(r[2], r[1], tags[1]);
    at(r[2], r[3], tags[2]);
    at(r[0], r[1], tags[3]);
    at(r[2], r[3], tags[4]);
    at(r[0], r[3], tags[5]);
}

void push_rect(cc::vector<corner>& out, tg::vec4f r, float depth, int tag)
{
    int const tags[] = {tag, tag, tag, tag, tag, tag};
    push_rect(out, r, depth, tags);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a flat member takes the first vertex of its triangle", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // One rect over a 4 × 4 target: the lower-right triangle's vertices are tagged 10, 11 and 12, the upper-left's
    // 20, 21 and 22, so a backend taking another provoking vertex reads 11, 12, 21 or 22.
    // (3, 2) and (0, 1) are off the shared diagonal, each well inside one triangle.
    auto vertices = cc::vector<corner>();
    int const tags[] = {10, 11, 12, 20, 21, 22};
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, tags);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics.flat_tags);

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = 4, .height = 4, .colors = {sg::pixel_format::rgba32_float}, .target_set = shaders::tag_target::name},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.bind_vertex_buffer(buffer.as_vertex_buffer());
            scope.draw({.vertex_range = {.offset = 0, .size = 6}});
        });

    CHECK(pixels[0].rgba_float(3, 2)[0] == 10.0f);
    CHECK(pixels[0].rgba_float(0, 1)[0] == 20.0f);
}

ASYNC_INVOCABLE_TEST("sg - a discarded pixel still takes part in its quad's derivatives", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 4 × 2 target, two quads wide: column 0 discards, and the rest write ddx(x²) at their centre.
    // Column 1's derivative spans the discarded column 0, so it reads 2 only where the discarded pixel went on as a
    // helper; columns 2 and 3 are the control, 6 whatever discard does.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics.discard_derivative);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 2,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::tag_target::name,
                                                          .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });

    for (auto y = 0; y < 2; ++y)
    {
        CHECK(pixels[0].rgba_float(0, y) == tg::vec4f(-1, -1, -1, -1)).context(cc::format("row {}", y));
        CHECK(pixels[0].rgba_float(1, y)[0] == 2.0f).context(cc::format("row {}", y));
        CHECK(pixels[0].rgba_float(2, y)[0] == 6.0f).context(cc::format("row {}", y));
        CHECK(pixels[0].rgba_float(3, y)[0] == 6.0f).context(cc::format("row {}", y));
    }
}

ASYNC_INVOCABLE_TEST("sg - a depth the pixel stage writes is the one tested, plain or promised to only move away",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 4 × 1 target whose depth is cleared to 0.5 and tested `less`, a column per case.
    // Each rect rasterizes at one depth and writes another, and only the written one decides:
    //  0: rasterized at 0.9, writes 0.25 -> drawn
    //  1: rasterized at 0.1, writes 0.75 -> not drawn
    //  2: as 1, promising the written depth is never nearer -> not drawn
    //  3: rasterized at 0.1, promising and writing 0.25 -> drawn
    struct column
    {
        float rasterized;
        int written; // the depth written, in hundredths
        bool pushed;
        bool drawn;
    };
    constexpr column columns[] = {
        {.rasterized = 0.9f, .written = 25, .pushed = false, .drawn = true},
        {.rasterized = 0.1f, .written = 75, .pushed = false, .drawn = false},
        {.rasterized = 0.1f, .written = 75, .pushed = true, .drawn = false},
        {.rasterized = 0.1f, .written = 25, .pushed = true, .drawn = true},
    };
    constexpr int width = 4;

    auto vertices = cc::vector<corner>();
    for (auto c = 0; c < width; ++c)
        push_rect(vertices, sg_test::clip_pixel(c, 0, width, 1), columns[c].rasterized, columns[c].written);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const plain = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics.depth_written);
    auto const pushed = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics.depth_pushed);

    // The two pipelines write different target sets, one per @pixel struct, so each draws in its own scope.
    auto const pixels
        = co_await sg_test::draw_offscreen_passes(*ctx,
                                                  {.width = width,
                                                   .height = 1,
                                                   .colors = {sg::pixel_format::rgba32_float},
                                                   .depth_stencil = sg::pixel_format::depth32_float,
                                                   .clear_depth = 0.5f},
                                                  [&](sg::command_list& cmd, sg_test::offscreen_targets const& targets)
                                                  {
                                                      auto first = targets.cleared();
                                                      first.target_set = shaders::written_depth::name;
                                                      {
                                                          auto scope = cmd.raster.render_to(first);
                                                          scope.bind_pipeline(*plain);
                                                          scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                          scope.draw({.vertex_range = {.offset = 0, .size = 12}});
                                                      }
                                                      auto second = targets.preserved();
                                                      second.target_set = shaders::pushed_depth::name;
                                                      auto scope = cmd.raster.render_to(second);
                                                      scope.bind_pipeline(*pushed);
                                                      scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                      scope.draw({.vertex_range = {.offset = 12, .size = 12}});
                                                  });

    for (auto c = 0; c < width; ++c)
        CHECK((pixels[0].rgba_float(c, 0)[0] == 1.0f) == columns[c].drawn).context(cc::format("column {}", c));
}

ASYNC_INVOCABLE_TEST("sg - a sample mask the pixel stage writes keeps the samples it leaves out",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // One pixel of a 4-sample target, cleared black, covered whole by a rect whose pixel stage writes mask 0b0101.
    // Samples 0 and 2 turn red and 1 and 3 stay black; a compute pass reads each sample back.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0b0101);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics.sample_masked);
    auto const read = co_await shaders::pixel_semantics.read_samples.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::samples>();

    auto const target
        = ctx->persistent.create_texture_2d_ms({.format = sg::pixel_format::rgba8_unorm,
                                                .width = 1,
                                                .height = 1,
                                                .sample_count = 4,
                                                .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::vector<float>::create_filled(4, -1.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))},
                                     .target_set = shaders::masked::name});
        scope.bind_pipeline(*draw);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::samples{.target = target.as_texture_view(), .values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*read);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == 4);
    CHECK(got[0] == 1.0f);
    CHECK(got[1] == 0.0f);
    CHECK(got[2] == 1.0f);
    CHECK(got[3] == 0.0f);
}

ASYNC_INVOCABLE_TEST("sg - gather returns a 2 x 2 footprint's texels in the one order every backend agrees on",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 2 × 2 texture, rows top to bottom: 51 102 / 153 204, which unorm reads as 0.2 0.4 / 0.6 0.8.
    // gather at its centre returns bottom-left, bottom-right, top-right, top-left: 0.6, 0.8, 0.4, 0.2.
    auto const pipeline = co_await shaders::pixel_semantics.gather_centre.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::gathered>();
    auto const source
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 2,
                                             .height = 2,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    u8 const texels[] = {51, 0, 0, 255, 102, 0, 0, 255, 153, 0, 0, 255, 204, 0, 0, 255};
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(1), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(source.raw(), cc::as_bytes(cc::span<u8 const>(texels)));
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::gathered{.source = source.as_texture_view(), .values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == 1);
    auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
    CHECK(near(got[0][0], 0.6f));
    CHECK(near(got[0][1], 0.8f));
    CHECK(near(got[0][2], 0.4f));
    CHECK(near(got[0][3], 0.2f));
}
