#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// A restart of the accumulation must not change what the image shows on average, only how noisy it is.
//
// The mean radiance of a mean of one sample per pixel is the converged image's mean, up to noise that thousands of pixels average away.
// So right after a restart, the accumulated image's mean has to match the one it converged to, and an automatic exposure,
// metered from that image, has to stay where it settled.
// A brightness that jumps on every small camera move is one or the other failing.

namespace
{
using namespace cc::primitive_defines;

/// How far the restarted image's mean may stray from the converged one, as a fraction of it.
constexpr f32 energy_tolerance = 0.10f;

/// How far the exposure may stray from where it settled, in stops.
constexpr f32 exposure_tolerance = 0.3f;

/// How many frames a view may take to converge, compiles included, before the test gives up on it.
/// A count rather than a time, since how long a frame takes is the GPU's and the load's.
constexpr i32 max_frames = 1024;

struct reading
{
    /// the accumulated image's mean luminance, linear
    f32 mean = 0;
    /// the automatic exposure, in stops, or 0 before a reading landed
    f32 exposure = 0;
    u32 samples = 0;
    /// whether the frame shaded through a stand-in for a permutation still compiling, whose image is not the converged one
    bool stand_ins = false;
};

[[nodiscard]] f32 mean_luminance(cc::span<tg::vec4f const> pixels)
{
    auto sum = 0.0;
    for (auto const& p : pixels)
        sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    return pixels.empty() ? 0.0f : f32(sum / f64(pixels.size()));
}

/// Converges one view drawn in `mode`, restarts its accumulation, and checks the frames after the restart.
cc::shared_async<cc::unit> check_restart(sg::context& ctx, sv::render_mode mode)
{
    auto resources = sv::gpu_resource_manager::create(ctx);
    // The lamp a mesh of its own, so it is the light the trace samples rather than the whole box's area.
    auto const lit = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 15.0f);
    auto const enclosure = resources.acquire_scene_item(lit.enclosure);
    auto const lamp = resources.acquire_scene_item(lit.lamp);
    resources.wait_for_pending_uploads();

    // Large enough that each of the exposure meter's cells averages dozens of pixels, as a window's do.
    auto const output_size = tg::vec2i(384, 384);
    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = output_size[0],
                                            .height = output_size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto index = u64(0);

    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("restarted");
        // Narrow enough that the box fills the view.
        v.camera = sv_test::cornell_camera();
        v.camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(enclosure);
        scene.items.push_back(lamp);
        scene.settings.mode = mode;
        scene.settings.motion_hold_ms = 0.0f;
        // One sample a frame is the noisiest restart, and a megakernel frame of more costs what the test does not need.
        scene.settings.samples_per_pixel = 1;
        scene.settings.reconstruct.denoiser = sr::denoise_method::none;
        scene.settings.display.automatic = true;
        def.views.push_back(cc::move(v));

        auto const root_node = def.nodes.add_container(sv::invalid_node);
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(0));
        def.nodes.add_leaf(root_node, cc::move(leaf));
        auto root = sv::view_data{};
        root.id = sv::view_id::from_string("root");
        root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
        def.root_view = sv::view_index(def.views.size());
        def.views.push_back(cc::move(root));
    }
    auto store = sv::view_store{};
    auto const history = sv::view_history{};
    auto const id = def.views[0].id;
    auto const camera = def.views[0].camera;

    // One frame, and what the view holds after it.
    auto const frame = [&]() -> cc::shared_async<reading>
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index++, history);
        (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto const* const state = store.peek_ptr(id);
        auto const* const slot = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
        auto accumulated = sg::data_future<tg::vec4f>();
        if (slot != nullptr && slot->texture.raw() != nullptr)
            accumulated = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot->texture.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        sv_test::drive_ambient_work();

        auto out = reading{.samples = store.min_accumulated_samples(id),
                           .stand_ins = slot == nullptr || slot->stand_in_hash != 0};
        if (accumulated.is_valid())
        {
            auto const pixels_async = accumulated.data();
            auto const& pixels = co_await pixels_async;
            out.mean = mean_luminance(cc::span<tg::vec4f const>(pixels.data(), pixels.size()));
        }
        if (state != nullptr && !state->auto_exposure.empty() && state->auto_exposure[0].primed)
            out.exposure = state->auto_exposure[0].exposure;
        co_return out;
    };

    // Converged: every compile landed, the mean holds enough samples, and the exposure has had time to settle.
    // Counted from the last frame that shaded through a stand-in, since its image is another one, and the exposure
    // meters it all the same: it moves a fifth of the way per reading, so it needs frames after the compile lands.
    // Counting stand-in frames let a late compile end the wait while the exposure was still adapting (measured).
    auto converged = reading{};
    auto drawn = 0;
    auto frames = 0;
    while (drawn < 128 || converged.samples < 64)
    {
        converged = co_await frame();
        drawn = converged.samples > 0 && !converged.stand_ins ? drawn + 1 : 0;
        REQUIRE(++frames < max_frames);
    }
    REQUIRE(converged.mean > 0.0f);

    // One frame a hair to the side restarts the mean, and the camera's return restarts it once more, for good.
    def.views[0].camera.position = camera.position + tg::vec3d(1e-3, 0, 0);
    (void)co_await frame();
    def.views[0].camera = camera;

    auto worst_energy = 0.0f;
    auto worst_energy_at = -1;
    auto worst_exposure = 0.0f;
    auto worst_exposure_at = -1;
    for (auto f = 0; f < 48; ++f)
    {
        auto const r = co_await frame();
        // A wavefront pixel holds nothing until its first path finishes, which is no sample of the mean yet.
        if (r.samples > 0)
        {
            auto const energy = tg::abs(r.mean - converged.mean) / converged.mean;
            if (energy > worst_energy)
            {
                worst_energy = energy;
                worst_energy_at = f;
            }
        }
        auto const exposure = tg::abs(r.exposure - converged.exposure);
        if (exposure > worst_exposure)
        {
            worst_exposure = exposure;
            worst_exposure_at = f;
        }
    }
    CC_LOG_INFO("mode {}: converged mean {:.4f} at exposure {:.2f}; after a restart, worst mean off by {:.1f}% at "
                "frame "
                "{}, worst exposure off by {:.2f} stops at frame {}",
                int(mode), converged.mean, converged.exposure, 100.0f * worst_energy, worst_energy_at, worst_exposure,
                worst_exposure_at);
    CHECK(worst_energy < energy_tolerance).dump("mode", int(mode)).dump("off by", worst_energy).dump("at", worst_energy_at);
    CHECK(worst_exposure < exposure_tolerance)
        .dump("mode", int(mode))
        .dump("off by", worst_exposure)
        .dump("at", worst_exposure_at);
    co_await cc::async_settled(sv::background_work(ctx));
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - restarting the accumulation keeps the image's mean and its automatic exposure, in the "
                     "hybrid",
                     (sg::context_handle const& ctx_h))
{
    if (!sv_test::can_run(*ctx_h) || !ctx_h->supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the view does not build");
    co_await check_restart(*ctx_h, sv::render_mode::hybrid);
}

ASYNC_INVOCABLE_TEST("sv - restarting the accumulation keeps the image's mean and its automatic exposure, in the "
                     "wavefront tracer",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    if (!sv_test::can_run(*ctx_h) || !ctx_h->supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the view does not build");
    if (nx::is_smoke())
        SKIP("a smoke run: the hybrid's restart covers what a preset's flags can break");
    co_await check_restart(*ctx_h, sv::render_mode::wavefront);
}

ASYNC_INVOCABLE_TEST("sv - restarting the accumulation keeps the image's mean and its automatic exposure, in the "
                     "megakernel",
                     (sg::context_handle const& ctx_h))
{
    if (!sv_test::can_run(*ctx_h) || !ctx_h->supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the view does not build");
    if (nx::is_smoke())
        SKIP("a smoke run: the hybrid's restart covers what a preset's flags can break");
    co_await check_restart(*ctx_h, sv::render_mode::megakernel);
}
