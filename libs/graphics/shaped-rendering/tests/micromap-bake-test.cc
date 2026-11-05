#include "shader_fixtures.hh"
#include "texel_formats.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/micromap_bake_routine.hh>
#include <sr_test_sgl_shaders.hh>

using namespace cc::primitive_defines;

// sr::micromap_bake_routine: opacity micromaps baked from a texture, checked by tracing the quad they were baked onto.
//
// The quad spans 8 x 8 units with the 8 x 8 texture stretched over it, opaque in its left half and transparent in its
// right, so a unit is a texel.
// A trace that ignores every candidate hits only what the bake marked opaque, and one that accepts every candidate
// misses only what it marked transparent.
// Bilinear lookups blend across the edge at x = 4, so the cells beside it are unknown and the shader decides them.

namespace
{
namespace probes = sr_test::sgl_shaders::micromap_trace;

/// Which of the 8 x 8 cells the trace of `entry` hit, row by row.
cc::shared_async<cc::vector<i32>> trace_baked(sg::context_handle ctx,
                                              sg::async_compute_pipeline entry,
                                              sg::sampler_address_mode address = sg::sampler_address_mode::clamp_edge,
                                              sg::sampler_filter filter = sg::sampler_filter::linear)
{
    sr::micromap_bake_routine::prewarm(*ctx);
    (void)co_await ctx->routines.idle_completion();
    auto const built = co_await entry;
    REQUIRE(built != nullptr);

    auto const opacity
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                             .width = 8,
                                             .height = 8,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto texels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < 8; ++y)
        for (auto x = 0; x < 8; ++x)
            texels.push_back(tg::vec4f(1, 1, 1, x < 4 ? 1.0f : 0.0f));

    // two triangles over the quad, whose texture coordinates are their positions over 8
    tg::vec2f const uvs[] = {tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(1, 1), tg::vec2f(0, 1)};
    u32 const indices[] = {0, 1, 2, 0, 2, 3};
    float const vertices[] = {0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0};

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(opacity.raw(), sr_test::packed(sg::pixel_format::rgba32_float, texels));
    auto const bake = sr::micromap_bake_routine::execute(*cmd, {.uvs = cc::span<tg::vec2f const>(uvs),
                                                                .indices = indices,
                                                                .opacity = opacity,
                                                                .address_u = address,
                                                                .address_v = address,
                                                                .filter = filter});
    REQUIRE(bake.has_value());
    auto const micromap = cmd->raytracing.build_micromap(bake.value().triangles);

    auto const input_usage = sg::buffer_usage::accel_structure_build_input;
    auto const vertex_buffer = ctx->persistent.create_buffer_from_data(vertices, input_usage);
    auto const index_buffer = ctx->persistent.create_buffer_from_data(indices, input_usage);
    sg::blas_triangles const geometry[] = {{.vertices = vertex_buffer.raw(),
                                            .vertex_count = 4,
                                            .indices = index_buffer.raw(),
                                            .index_count = 6,
                                            .is_opaque = false,
                                            .micromap = micromap,
                                            .micromap_indices = bake.value().indices}};
    auto const blas = cmd->raytracing.build_blas(geometry);
    sg::tlas_instance const placed[] = {{.blas = blas, .cull_mode = sg::instance_cull_mode::none}};
    auto const tlas = cmd->raytracing.build_tlas(placed);

    auto const hits = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(64), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<probes::probe>(),
        probes::probe{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(8, 8, 1);
    auto const readback = cmd->download.data_from_buffer(hits);
    ctx->submit_command_list(cc::move(cmd));
    ctx->advance_epoch();

    auto const& data = co_await readback.data();
    auto cells = cc::vector<i32>();
    for (auto const v : data)
        cells.push_back(v);
    co_return cells;
}

/// The columns a row of `cells` is hit in, as a string of 0 and 1, the same for every row or `mixed`.
cc::string columns_hit(cc::span<i32 const> cells)
{
    auto first = cc::string();
    for (auto y = 0; y < 8; ++y)
    {
        auto row = cc::string();
        for (auto x = 0; x < 8; ++x)
            row += cells[y * 8 + x] != 0 ? '1' : '0';
        if (y == 0)
            first = row;
        else if (row != first)
            return "mixed";
    }
    return first;
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - a baked micromap decides the texels its cutoff decides, and leaves the edge to the shader",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    (void)sr_test::shader_fixtures();
    if (!ctx->supports(sg::feature::opacity_micromap))
        SKIP("this device has no opacity micromaps");

    // opaque from the left edge to the texel before the blend, everything else asks and is ignored
    auto const rejected = co_await trace_baked(ctx, probes::rejecting_cs.acquire_pipeline(*ctx));
    CHECK(columns_hit(rejected) == "11100000");
    // transparent from the texel past the blend, everything else asks and is accepted
    auto const accepted = co_await trace_baked(ctx, probes::accepting_cs.acquire_pipeline(*ctx));
    CHECK(columns_hit(accepted) == "11111000");
}

ASYNC_INVOCABLE_TEST("sr - a micromap baked for a nearest or a wrapping sampler reads what that sampler reads",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    (void)sr_test::shader_fixtures();
    if (!ctx->supports(sg::feature::opacity_micromap))
        SKIP("this device has no opacity micromaps");
    using mode = sg::sampler_address_mode;

    // a nearest lookup blends nothing across the edge at x = 4, so every texel is decided and the shader is never asked
    auto const nearest_rejected = co_await trace_baked(ctx, probes::rejecting_cs.acquire_pipeline(*ctx),
                                                       mode::clamp_edge, sg::sampler_filter::nearest);
    CHECK(columns_hit(nearest_rejected) == "11110000");
    auto const nearest_accepted = co_await trace_baked(ctx, probes::accepting_cs.acquire_pipeline(*ctx),
                                                       mode::clamp_edge, sg::sampler_filter::nearest);
    CHECK(columns_hit(nearest_accepted) == "11110000");

    // a repeating linear lookup at the left edge blends in the transparent right column, so column 0 asks too
    auto const repeated = co_await trace_baked(ctx, probes::rejecting_cs.acquire_pipeline(*ctx), mode::repeat);
    CHECK(columns_hit(repeated) == "01100000");
    // a mirroring one reads the opaque left column again, as clamping does
    auto const mirrored = co_await trace_baked(ctx, probes::rejecting_cs.acquire_pipeline(*ctx), mode::mirror_repeat);
    CHECK(columns_hit(mirrored) == "11100000");
}

ASYNC_INVOCABLE_TEST("sr - a ray forcing a baked micromap to 2 states takes the guesses, and asks nothing",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    (void)sr_test::shader_fixtures();
    if (!ctx->supports(sg::feature::opacity_micromap))
        SKIP("this device has no opacity micromaps");

    // the edge's unknown cells guess by the texel at their centre: 3 is opaque and 4 transparent, whatever the decision
    auto const rejected = co_await trace_baked(ctx, probes::rejecting_2_state_cs.acquire_pipeline(*ctx));
    CHECK(columns_hit(rejected) == "11110000");
    auto const accepted = co_await trace_baked(ctx, probes::accepting_2_state_cs.acquire_pipeline(*ctx));
    CHECK(columns_hit(accepted) == "11110000");
}

TEST("sr - a triangle's micromap level gives it about one micro-triangle per texel it covers")
{
    // half of a 64 x 64 texture is 2048 texels, which 4^6 = 4096 micro-triangles first reach
    auto const o = tg::vec2f(0, 0);
    CHECK(sr::micromap_bake_routine::level_for(o, tg::vec2f(1, 0), tg::vec2f(1, 1), 64, 64, 12) == 6);
    CHECK(sr::micromap_bake_routine::level_for(o, tg::vec2f(1, 0), tg::vec2f(1, 1), 64, 64, 4) == 4);
    // a triangle smaller than a texel needs no subdivision, and a degenerate one none either
    CHECK(sr::micromap_bake_routine::level_for(o, tg::vec2f(0.001f, 0), tg::vec2f(0, 0.001f), 64, 64, 12) == 0);
    CHECK(sr::micromap_bake_routine::level_for(o, tg::vec2f(1, 1), tg::vec2f(0.5f, 0.5f), 64, 64, 12) == 0);
}
