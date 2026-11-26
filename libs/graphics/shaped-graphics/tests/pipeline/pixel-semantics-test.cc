#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/known_issues.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// What SGL states for every target and only a GPU shows a backend doing: pixel_semantics.sgl's stages, drawn and read.

namespace
{
using corner = shaders::pixel_semantics::tagged_corner;

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

    // One rect over a 4 × 4 target: the lower-right triangle's vertices are tagged 10, 11 and 12, the upper-left's 20, 21 and 22.
    // So a backend taking another provoking vertex reads 11, 12, 21 or 22.
    // (3, 2) and (0, 1) are off the shared diagonal, each well inside one triangle.
    auto vertices = cc::vector<corner>();
    int const tags[] = {10, 11, 12, 20, 21, 22};
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, tags);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::flat_tags);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 4,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name},
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
    // Column 1's derivative spans the discarded column 0, so it reads 2 only where the discarded pixel went on as a helper.
    // Columns 2 and 3 are the control, 6 whatever discard does.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::discard_derivative);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 2,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name,
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

ASYNC_INVOCABLE_TEST("sg - a quad vote is its whole quad's, so a derivative under a branch on one is its quad's "
                     "(CHK-533)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::quad_control))
        SKIP("this context has no quad votes its helpers take part in");

    // An 8 x 4 target, four quads wide and two high: only pixel (5, 2) asks, so only its quad takes the derivative.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::quad_votes);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 8,
                                                          .height = 4,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name,
                                                          .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });

    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 8; ++x)
        {
            auto const in_asking_quad = x / 2 == 2 && y / 2 == 1;
            auto const got = pixels[0].rgba_float(x, y);
            CHECK(got[0] == (in_asking_quad ? 1.0f : 0.0f)).context(cc::format("quad_any at ({}, {})", x, y));
            CHECK(got[1] == (x < 4 ? 1.0f : 0.0f)).context(cc::format("quad_all at ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - a coarse derivative is one per quad and a fine one per pixel, at every float level",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 4 × 4 target, two quads by two, each pixel writing ddx, ddy, ddx_fine and ddy_fine of x·y at its centre.
    // A fine derivative along x is the pixel's own row's y, along y its own column's x.
    // A coarse one is the same in all four pixels of a quad, one of its two rows or columns: which one, a target leaves open.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);

    for (auto const fast : {false, true})
    {
        auto const pipeline
            = fast ? co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::fast_quad_derivatives)
                   : co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::quad_derivatives);
        auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                             {.width = 4,
                                                              .height = 4,
                                                              .colors = {sg::pixel_format::rgba32_float},
                                                              .target_set = shaders::pixel_semantics::tag_target::name},
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                             });

        for (auto y = 0; y < 4; ++y)
            for (auto x = 0; x < 4; ++x)
            {
                auto const at = cc::format("{} pixel ({}, {})", fast ? ".fast" : "default", x, y);
                auto const got = pixels[0].rgba_float(x, y);
                auto const first = pixels[0].rgba_float(x & ~1, y & ~1);
                auto const row = float(y & ~1);
                auto const column = float(x & ~1);
                CHECK(got[0] == first[0]).context(at);
                CHECK((got[0] == row + 0.5f || got[0] == row + 1.5f)).context(at).context(cc::format("ddx {}", got[0]));
                CHECK(got[1] == first[1]).context(at);
                CHECK((got[1] == column + 0.5f || got[1] == column + 1.5f)).context(at).context(cc::format("ddy {}", got[1]));
                CHECK(got[2] == float(y) + 0.5f).context(at);
                CHECK(got[3] == float(x) + 0.5f).context(at);
            }
    }
}

ASYNC_INVOCABLE_TEST("sg - a strong compare-exchange in a pixel stage leaves helper pixels out, and the draw ends",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::helper_lane_query))
        SKIP("this context cannot tell a helper invocation, which a pixel stage's strong compare-exchange needs");

    // `derivative_ps`'s 4 × 2 target, where each pixel claims its own slot before the derivative.
    // Column 0 discards first and goes on as helpers, whose compare-exchange stores nothing and must not loop forever.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 6);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::discard_claim);
    auto const owners = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(8), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::pixel_claims>(),
        shaders::pixel_semantics::pixel_claims{.owners = owners.as_readwrite_buffer()});

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 2,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name,
                                                          .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_group(0, *group);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });
    auto cmd = ctx->create_command_list();
    auto const back = cmd->download.data_from_buffer(owners);
    ctx->submit_command_list(cc::move(cmd));
    auto const got = co_await back.data();
    REQUIRE(got.size() == 8);

    // each drawn pixel found its slot empty and claimed it; the derivative still saw the helper, as without the claim
    for (auto y = 0; y < 2; ++y)
    {
        CHECK(pixels[0].rgba_float(0, y) == tg::vec4f(-1, -1, -1, -1)).context(cc::format("row {}", y));
        CHECK(got[4 * y] == 0u).context(cc::format("row {}", y));
        CHECK(pixels[0].rgba_float(1, y)[0] == 2.0f).context(cc::format("row {}", y));
        for (auto x = 1; x < 4; ++x)
        {
            CHECK(pixels[0].rgba_float(x, y)[1] == 0.0f).context(cc::format("pixel ({}, {})", x, y));
            CHECK(got[x + 4 * y] == 7u).context(cc::format("pixel ({}, {})", x, y));
        }
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
    auto const plain = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::depth_written);
    auto const pushed = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::depth_pushed);

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
                                                      first.target_set = shaders::pixel_semantics::written_depth::name;
                                                      {
                                                          auto scope = cmd.raster.render_to(first);
                                                          scope.bind_pipeline(*plain);
                                                          scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                          scope.draw({.vertex_range = {.offset = 0, .size = 12}});
                                                      }
                                                      auto second = targets.preserved();
                                                      second.target_set = shaders::pixel_semantics::pushed_depth::name;
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
    auto const draw = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::sample_masked);
    auto const read = co_await shaders::pixel_semantics::read_samples.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::samples>();

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
                                     .target_set = shaders::pixel_semantics::masked::name});
        scope.bind_pipeline(*draw);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::pixel_semantics::samples{.target = target.as_texture_view(), .values = values.as_readwrite_buffer()});
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
    auto const pipeline = co_await shaders::pixel_semantics::gather_centre.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::gathered>();
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
        *cmd, layout,
        shaders::pixel_semantics::gathered{.source = source.as_texture_view(), .values = values.as_readwrite_buffer()});
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

ASYNC_INVOCABLE_TEST("sg - a pixel stage returns one color without a struct, or nothing and writes memory alone",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // CHK-418: `direct_ps` returns its float4, which is the one target of the struct SGL mints for it
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const direct = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::direct_tags);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 2,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::direct_ps_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*direct);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });
    CHECK(pixels[0].rgba_float(1, 1) == tg::vec4f(7, 1, 0, 1));

    // `marking_ps` returns nothing: its pipeline has a depth target alone, and every pixel marks its slot
    auto const marking = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::marked);
    auto const marks = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(8), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::pixel_marks>(),
        shaders::pixel_semantics::pixel_marks{.marks = marks.as_readwrite_buffer()});
    (void)co_await sg_test::draw_offscreen(*ctx,
                                           {.width = 4, .height = 2, .depth_stencil = sg::pixel_format::depth32_float},
                                           [&](sg::rendering_scope& scope)
                                           {
                                               scope.bind_pipeline(*marking);
                                               scope.bind_group(0, *group);
                                               scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                               scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                           });
    auto cmd = ctx->create_command_list();
    auto const back = cmd->download.data_from_buffer(marks);
    ctx->submit_command_list(cc::move(cmd));
    auto const got = co_await back.data();
    REQUIRE(got.size() == 8);
    for (auto i = 0; i < 8; ++i)
        CHECK(got[i] == 8u).context(cc::format("slot {}", i));
}

ASYNC_INVOCABLE_TEST("sg - a pixel stage asks which level of detail a sample would read, clamped and not (CHK-535)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 64 x 64 texture of 7 levels over a 16 x 16 target: 4 texels a pixel, level 2.
    // An eighth of that is half a texel a pixel, level -1, which the clamped query raises to 0.
    // Hardware keeps a level to a few bits of fraction, so each is held to within 1/16.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::lod_query);
    auto const source
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 64,
                                             .height = 64,
                                             .mip_levels = 7,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::detail>(),
        shaders::pixel_semantics::detail{.source = source.as_texture_view()});

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 16,
                                                          .height = 16,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_group(0, *group);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });

    constexpr float expected[] = {2.0f, 2.0f, 0.0f, -1.0f};
    constexpr cc::string_view names[] = {"lod", "lod_unclamped", "lod of an eighth", "lod_unclamped of an eighth"};
    for (auto y = 0; y < 16; ++y)
        for (auto x = 0; x < 16; ++x)
        {
            auto const got = pixels[0].rgba_float(x, y);
            for (auto i = 0; i < 4; ++i)
            {
                auto const within = got[i] - expected[i] <= 1.0f / 16 && expected[i] - got[i] <= 1.0f / 16;
                CHECK(within).context(cc::format("{} at ({}, {}) is {}, not {}", names[i], x, y, got[i], expected[i]));
            }
        }
}

ASYNC_INVOCABLE_TEST("sg - a vertex stage sends each instance to its own viewport, each with its own scissor (CHK-534)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::multi_viewport))
        SKIP("this context has one viewport alone");

    // One rect over the whole of clip space, drawn twice: instance 0 into the left half of an 8 x 4 target, instance 1 into the right half.
    // Each instance raises its tag by 10, and one viewport alone would leave the right half clear.
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 3);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::placed_tags);

    sg::viewport const halves[] = {
        {.offset = tg::pos2f(0, 0), .size = tg::vec2f(4, 4)},
        {.offset = tg::pos2f(4, 0), .size = tg::vec2f(4, 4)},
    };
    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = 8, .height = 4, .colors = {sg::pixel_format::rgba32_float}, .clear_color = tg::vec4f(-1, -1, -1, -1)},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.set_viewports(halves);
            scope.bind_vertex_buffer(buffer.as_vertex_buffer());
            scope.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = 0, .size = 2}});
        });
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 8; ++x)
        {
            auto const expected = x < 4 ? tg::vec4f(3, 0, 0, 1) : tg::vec4f(13, 1, 0, 1);
            CHECK(pixels[0].rgba_float(x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }

    // a scissor per viewport: the right one cut to its top row alone
    tg::aabb2i const cuts[]
        = {tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(4, 4)), tg::aabb2i(tg::pos2i(4, 0), tg::pos2i(8, 1))};
    auto const cut = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = 8, .height = 4, .colors = {sg::pixel_format::rgba32_float}, .clear_color = tg::vec4f(-1, -1, -1, -1)},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.set_viewports(halves);
            scope.set_scissors(cuts);
            scope.bind_vertex_buffer(buffer.as_vertex_buffer());
            scope.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = 0, .size = 2}});
        });
    CHECK(cut[0].rgba_float(5, 0) == tg::vec4f(13, 1, 0, 1));
    CHECK(cut[0].rgba_float(5, 2) == tg::vec4f(-1, -1, -1, -1));
    CHECK(cut[0].rgba_float(1, 2) == tg::vec4f(3, 0, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - a vertex stage sends each instance to its own layer of an array target",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::vertex_layer_output))
        SKIP("this context cannot pick a layer from the vertex stage");

    // CHK-419: two instances of one rect, the second sent to layer 1 with its tag raised by 10
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::layered_tags);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .target_set = shaders::pixel_semantics::layered_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = 0, .size = 2}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const layer_0 = co_await first.bytes();
    auto const layer_1 = co_await second.bytes();
    REQUIRE(layer_0.size() == 4 * 2 * isize(sizeof(tg::vec4f)));
    REQUIRE(layer_1.size() == layer_0.size());
    CHECK(texel(layer_0, 1, 1) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(layer_1, 1, 1) == tg::vec4f(17, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - one draw of two views puts the same geometry somewhere else in each",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::view_instancing))
        SKIP("this context renders one view per draw");

    // a full-screen rect the vertex stage squeezes into the left half of view 0 and the right half of view 1
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::moved_views);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .view_count = 2,
                                     .target_set = shaders::pixel_semantics::viewed_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const view_0 = co_await first.bytes();
    auto const view_1 = co_await second.bytes();
    CHECK(texel(view_0, 0, 0) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(view_0, 3, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 0, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 3, 0) == tg::vec4f(7, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - one draw of two views moves geometry with only the vertex stage reading which",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::view_instancing))
        SKIP("this context renders one view per draw");
    if (auto const* issue = sg::known_issue_affecting(*ctx, "intel-d3d12-view-instancing-draws-nothing"))
        nx::skip_known_issue(issue->id, issue->symptom);

    nx::allow_warnings("takes no `@view_index`", "slib");

    // a full-screen rect the vertex stage squeezes into the left half of view 0 and the right half of view 1
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::view_unread::moved_vertex_stage_only);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .view_count = 2,
                                     .target_set = shaders::view_unread::plain_tag_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const view_0 = co_await first.bytes();
    auto const view_1 = co_await second.bytes();
    CHECK(texel(view_0, 0, 0) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(view_0, 3, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 0, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 3, 0) == tg::vec4f(7, 0, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - one draw of two views puts geometry from its vertex index somewhere else in each",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::view_instancing))
        SKIP("this context renders one view per draw");

    // no vertex buffer: the vertex stage places a strip in the left half of view 0 and the right half of view 1
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::moved_index_views);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .view_count = 2,
                                     .target_set = shaders::pixel_semantics::viewed_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.draw({.vertex_range = {.offset = 0, .size = 4}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const view_0 = co_await first.bytes();
    auto const view_1 = co_await second.bytes();
    CHECK(texel(view_0, 0, 0) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(view_0, 3, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 0, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 3, 0) == tg::vec4f(7, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - one draw of two views puts the same geometry somewhere else in each, over a depth array",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::view_instancing))
        SKIP("this context renders one view per draw");

    // a full-screen rect the vertex stage squeezes into the left half of view 0 and the right half of view 1
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::moved_views_depth);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto const depth = ctx->persistent.create_texture_2d_array({.format = sg::pixel_format::depth32_float,
                                                                .width = 4,
                                                                .height = 2,
                                                                .array_layers = 2,
                                                                .usage = sg::texture_usage::depth_stencil});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f),
                                     .view_count = 2,
                                     .target_set = shaders::pixel_semantics::viewed_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const view_0 = co_await first.bytes();
    auto const view_1 = co_await second.bytes();
    CHECK(texel(view_0, 0, 0) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(view_0, 3, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 0, 0) == tg::vec4f(-1, -1, -1, -1));
    CHECK(texel(view_1, 3, 0) == tg::vec4f(7, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - one draw of two views renders each into its own layer, and both stages know which",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::view_instancing))
        SKIP("this context renders one view per draw");

    // CHK-437: one instance of one rect, which view 1 raises by 10, and the pixel stage reports its view
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 7);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::viewed_tags);
    auto const target = ctx->persistent.create_texture_2d_array(
        {.format = sg::pixel_format::rgba32_float,
         .width = 4,
         .height = 2,
         .array_layers = 2,
         .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(-1, -1, -1, -1))},
                                     .view_count = 2,
                                     .target_set = shaders::pixel_semantics::viewed_ps_target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_vertex_buffer(buffer.as_vertex_buffer());
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const first = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 0});
    auto const second = cmd->download.bytes_from_texture(target.raw(), {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const texel = [](cc::span<byte const> bytes, int x, int y)
    {
        auto value = tg::vec4f();
        cc::memcpy(&value, bytes.data() + (isize(y) * 4 + x) * isize(sizeof(tg::vec4f)), sizeof(tg::vec4f));
        return value;
    };
    auto const view_0 = co_await first.bytes();
    auto const view_1 = co_await second.bytes();
    REQUIRE(view_0.size() == 4 * 2 * isize(sizeof(tg::vec4f)));
    REQUIRE(view_1.size() == view_0.size());
    CHECK(texel(view_0, 1, 1) == tg::vec4f(7, 0, 0, 1));
    CHECK(texel(view_1, 1, 1) == tg::vec4f(17, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - an SGL pixel stage with @early_depth_test runs for no pixel that fails the depth test",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::early_depth_test))
        SKIP("this context runs no depth test before a pixel stage that writes memory");

    // A 4 x 4 target whose depth is cleared to 0.5, and a rect over it at 0.75, which the `less` test fails everywhere.
    // The early stage counts no pixel; the late one runs for all 16, since a stage that writes memory is tested after.
    constexpr int side = 4;
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.75f, 1);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const early = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::counted_early);
    auto const late = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::counted_late);
    // a buffer each, so the second draw waits on no barrier for the first's writes
    auto const make_counts = [&]
    {
        return ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(2),
                                                       sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    };
    auto const early_counts = make_counts();
    auto const late_counts = make_counts();
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::pixel_semantics::early_counts>();
    auto const early_group = ctx->persistent.create_binding_group(
        layout, shaders::pixel_semantics::early_counts{.counted = early_counts.as_readwrite_buffer()});
    auto const late_group = ctx->persistent.create_binding_group(
        layout, shaders::pixel_semantics::early_counts{.counted = late_counts.as_readwrite_buffer()});

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = side,
                                                          .height = side,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .depth_stencil = sg::pixel_format::depth32_float,
                                                          .target_set = shaders::pixel_semantics::tag_target::name,
                                                          .clear_depth = 0.5f},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*early);
                                                             scope.bind_group(0, *early_group);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                             scope.bind_pipeline(*late);
                                                             scope.bind_group(0, *late_group);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });
    auto cmd = ctx->create_command_list();
    auto const early_back = cmd->download.data_from_buffer(early_counts);
    auto const late_back = cmd->download.data_from_buffer(late_counts);
    ctx->submit_command_list(cc::move(cmd));
    auto const early_got = co_await early_back.data();
    auto const late_got = co_await late_back.data();
    REQUIRE(early_got.size() == 2);
    REQUIRE(late_got.size() == 2);
    CHECK(early_got[0] == 0u);
    CHECK(late_got[1] == u32(side * side));
    // neither draw passed the test, so no color reached the target
    CHECK(pixels[0].rgba_float(1, 1) == tg::vec4f(0, 0, 0, 0));
}

ASYNC_INVOCABLE_TEST("sg - an SGL vertex stage's clip distances cut what the rasterizer draws",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::clip_distances))
        SKIP("this context clips by no distance a vertex stage writes");

    // a rect over a 4 x 1 target, whose sixth clip distance is x: the two pixels left of the centre are cut away
    auto vertices = cc::vector<corner>();
    push_rect(vertices, tg::vec4f(-1, -1, 1, 1), 0.5f, 0);
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(vertices), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::pixel_semantics::clipped_draw);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 1,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::pixel_semantics::tag_target::name,
                                                          .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });
    for (auto x = 0; x < 4; ++x)
        CHECK(pixels[0].rgba_float(x, 0) == (x < 2 ? tg::vec4f(-1, -1, -1, -1) : tg::vec4f(1, 0, 0, 1)))
            .context(cc::format("pixel {}", x));
}
