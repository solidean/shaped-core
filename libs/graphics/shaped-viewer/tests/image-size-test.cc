#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

// Images of sizes no test otherwise draws: a pixel, a sliver, odd extents that divide by no tile, and both orientations of each.
// Dispatch sizes, the raster's culling pyramid and an upscaler's input extent are each where an off-by-one hides.
// Every frame read is checked for non-finite pixels, as the agreement harness checks every frame it reads.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

constexpr tg::vec2i k_sizes[]
    = {tg::vec2i(1, 1), tg::vec2i(3, 2), tg::vec2i(97, 13), tg::vec2i(13, 97), tg::vec2i(1, 64), tg::vec2i(64, 1)};

[[nodiscard]] f64 mean_luminance(cc::span<tg::vec4f const> image)
{
    auto total = 0.0;
    for (auto const& c : image)
        total += agreement::luminance_of(c);
    return total / f64(cc::max(image.size(), isize(1)));
}
} // namespace

ASYNC_INVOCABLE_TEST("sv size - every mode draws a lit, finite image at sizes no tile divides",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);

    agreement::mode_config const modes[] = {
        {.name = "size megakernel", .mode = sv::render_mode::megakernel},
        {.name = "size wavefront", .mode = sv::render_mode::wavefront},
        {.name = "size forward", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward},
        {.name = "size deferred", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred},
    };
    for (auto const& m : modes)
        for (auto const size : k_sizes)
        {
            // A pixel or two sees one surface, the same in both halves where it is one the lamp lights directly.
            // A wide sliver sees far past the box at the vertical field of view, mostly into the black around it, so lit means lit at all.
            auto const c = co_await agreement::converge(ctx, resources, box.items, box.camera, m, 16, 0, size, true);
            REQUIRE(c.first.size() == isize(size[0]) * size[1]).dump("mode", m.name).dump("width", size[0]);
            auto const brightness = mean_luminance(c.first);
            CHECK(brightness > 0.001).dump("mode", m.name).dump("width", size[0]).dump("height", size[1]).dump("mean", brightness);
        }
    co_await cc::async_settled(sv::background_work(ctx));
}

// A moving camera traced at each upscaler preset's input extent and upscaled to an odd output: the input extent rounds,
// and an extent of no pixel, or one the upscaler and the trace disagree on, is what this would find.
ASYNC_INVOCABLE_TEST("sv size - a moving image upscales from every preset's extent to an odd output",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    if (!sr::query_reconstruct_support(ctx).fsr)
        SKIP("no upscaler on this device");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);

    sr::render_scale_preset const presets[]
        = {sr::render_scale_preset::quality, sr::render_scale_preset::balanced, sr::render_scale_preset::performance};
    constexpr tg::vec2i outputs[] = {tg::vec2i(97, 13), tg::vec2i(3, 2), tg::vec2i(1, 1)};
    for (auto const mode : {sv::render_mode::megakernel, sv::render_mode::hybrid})
        for (auto const preset : presets)
            for (auto const output_size : outputs)
            {
                CC_LOG_INFO("size moving: {} at preset {}, {}x{}",
                            mode == sv::render_mode::hybrid ? "hybrid" : "megakernel", int(preset), output_size[0],
                            output_size[1]);
                auto def = agreement::definition_of(box.items, box.camera, {.name = "size moving", .mode = mode});
                auto& settings = sv::ensure_scene_3d(def.views[0]).settings;
                settings.reconstruct
                    = {.denoiser = sr::denoise_method::svgf, .upscaler = sr::upscale_method::fsr, .scale = preset};
                auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                                      .width = output_size[0],
                                                                      .height = output_size[1],
                                                                      .usage = sg::texture_usage::render_target});
                auto store = sv::view_store{};
                auto const history = sv::view_history{};
                auto executed = 0;
                // Compiles first, then a move: every frame a step to the side, so every one traces at the input extent.
                for (auto f = 0; f < 16384 && executed < 8; ++f)
                {
                    (void)ctx.routines.tick();
                    if (executed > 0)
                        def.views[0].camera.position += tg::vec3d(0.01, 0, 0);
                    auto cmd = ctx.create_command_list();
                    resources.advance_to(ctx.current_epoch());
                    store.begin_frame(u64(ctx.current_epoch()));
                    auto const plan = sv::build_render_plan(def, output_size, u64(f), history);
                    auto const outcome = sv::viewer_renderer::execute(
                        *cmd, def, plan, resources, store, output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                    ctx.submit_command_list(cc::move(cmd));
                    ctx.advance_epoch();
                    co_await ctx.idle_completion();
                    sv_test::drive_ambient_work();
                    executed += outcome == sg::routine_outcome::executed ? 1 : 0;
                }
                CHECK(executed == 8)
                    .dump("mode", mode == sv::render_mode::hybrid ? "hybrid" : "megakernel")
                    .dump("preset", int(preset))
                    .dump("width", output_size[0])
                    .dump("height", output_size[1]);
                co_await cc::async_settled(sv::background_work(ctx));
            }
}
