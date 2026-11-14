#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// A viewer driven through the switches a person makes in a panel, frame by frame: the renderer, the denoiser, the
// temporal schedule, and the camera moving and stopping.
//
// Two things must hold on every frame once the viewer has shown anything at all.
// No block of the image goes black: the Cornell box fills the view and its lamp lights all but the shadow under the
// blocks, so more near-black pixels than that is a region drawn from a target nothing wrote, or a frame of a scene still
// waiting on its lamp.
// And a still image settles: once the camera stops, consecutive frames differ less and less, and the image keeps its
// brightness, which a denoiser fed its own output would not.

namespace
{
using namespace cc::primitive_defines;

/// What one frame looked like, in 8-bit display values.
struct frame_stats
{
    f32 mean = 0;
    /// the share of pixels near black in every channel
    f32 dark = 0;
    /// the mean absolute difference to the frame before, per channel
    f32 change = 0;
};

/// One phase of the script: the settings it switches to, whether the camera moves through it, and how many frames it
/// executes.
/// A frame declined while a compile runs is waited out on top, so a phase settles however fast frames are.
struct phase
{
    char const* name;
    sv::render_mode mode;
    sv::hybrid_lighting lighting;
    sr::denoise_method denoiser;
    u32 temporal_frames;
    bool moving;
    int frames;
    /// `render_settings::hybrid_bounces`
    int bounces = 1;
};

constexpr u32 forever = u32(-1);

/// The share of near-black pixels a lit frame holds at most: the shadow under the blocks is about 2.5% of this view.
constexpr f32 dark_ceiling = 0.05f;

constexpr phase k_script[] = {
    {"deferred, automatic", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, 16,
     false, 48},
    {"deferred, raw mean", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::none, 16, false,
     64},
    {"deferred, atrous", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::atrous, 16, false,
     64},
    {"forward", sv::render_mode::hybrid, sv::hybrid_lighting::forward, sr::denoise_method::automatic, 16, false, 64},
    {"path traced", sv::render_mode::wavefront, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, 16, false,
     48},
    {"deferred again, temporal forever", sv::render_mode::hybrid, sv::hybrid_lighting::deferred,
     sr::denoise_method::automatic, forever, false, 64},
    {"svgf, forever", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::svgf, forever, false,
     48},
    {"dlss_rr, forever", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::dlss_rr, forever,
     false, 48},
    {"moving", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, forever, true, 24},
    {"stopped, forever", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, forever,
     false, 64},
    {"stopped, handed over", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, 16,
     false, 64},
    {"deferred, two bounces", sv::render_mode::hybrid, sv::hybrid_lighting::deferred, sr::denoise_method::automatic, 16,
     false, 64, 2},
    {"forward, two bounces", sv::render_mode::hybrid, sv::hybrid_lighting::forward, sr::denoise_method::automatic, 16,
     false, 400, 2},
};

[[nodiscard]] frame_stats stats_of(cc::span<u32 const> pixels, cc::span<u32 const> previous)
{
    auto out = frame_stats{};
    auto dark = 0;
    auto change = 0.0f;
    for (auto i = isize(0); i < pixels.size(); ++i)
    {
        auto const p = pixels[i];
        auto const b = int(p & 0xff);
        auto const g = int((p >> 8) & 0xff);
        auto const r = int((p >> 16) & 0xff);
        out.mean += f32(r + g + b) / 3.0f;
        if (r < 6 && g < 6 && b < 6)
            ++dark;
        if (!previous.empty())
        {
            auto const q = previous[i];
            change += f32(tg::abs(b - int(q & 0xff)) + tg::abs(g - int((q >> 8) & 0xff))
                          + tg::abs(r - int((q >> 16) & 0xff)))
                    / 3.0f;
        }
    }
    auto const n = f32(pixels.size());
    out.mean /= n;
    out.dark = f32(dark) / n;
    out.change = previous.empty() ? 0.0f : change / n;
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - switching renderers and denoisers never shows black blocks, and a still image settles",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");
    // The dlss_rr phase falls back where the SDK was not fetched or the adapter is no RTX, and says so once.
    nx::allow_warnings("denoiser 'dlss_rr' did not run");

    auto resources = sv::gpu_resource_manager::create(ctx);
    // The lamp a mesh of its own, so it is the light the trace samples rather than the whole box's area.
    auto const lit = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 15.0f);
    auto const enclosure = resources.acquire_scene_item(lit.enclosure);
    auto const lamp = resources.acquire_scene_item(lit.lamp);
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(96, 96);
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("switched");
        // Narrower than the other Cornell box tests look, so the box fills the whole view and no pixel sees past its edges.
        v.camera = sv_test::cornell_camera();
        v.camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(enclosure);
        scene.items.push_back(lamp);
        scene.settings.motion_hold_ms = 0.0f;
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

    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = output_size[0],
                                            .height = output_size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto store = sv::view_store{};
    auto const history = sv::view_history{};
    auto index = u64(0);
    auto previous = cc::vector<u32>();
    auto const start_camera = def.views[0].camera;
    // Until the first frame executes there is no earlier image to hold, so the output shows what it was cleared to.
    auto shown = false;

    // a smoke run plays the first phases, which still switch renderers, and what a preset's flags can break
    auto const script = nx::is_smoke() ? cc::span<phase const>(k_script).subspan({.offset = 0, .size = 3})
                                       : cc::span<phase const>(k_script);
    for (auto const& p : script)
    {
        auto& settings = def.views[0].layers[0].settings;
        settings.mode = p.mode;
        settings.hybrid_lighting = p.lighting;
        settings.reconstruct.denoiser = p.denoiser;
        settings.temporal_denoise_frames = p.temporal_frames;
        settings.hybrid_bounces = p.bounces;

        auto stats = cc::vector<frame_stats>();
        // Only frames that executed settle: one that declined shows an image held over, or one drawn through stand-ins.
        auto executed = cc::vector<frame_stats>();
        auto skipped = 0;
        auto worst_dark = 0.0f;
        auto worst_at = -1;
        auto const phase_start = cc::current_time_steady_secs();
        for (auto f = 0; executed.size() < isize(p.frames); ++f)
        {
            // Before the list: a REQUIRE unwinding past an unsubmitted one asserts in its destructor.
            REQUIRE(cc::current_time_steady_secs() - phase_start < 60.0);
            if (p.moving)
                def.views[0].camera.position = start_camera.position + tg::vec3d(0.01 * f, 0, 0);

            (void)ctx.routines.tick();
            auto cmd = ctx.create_command_list();
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            auto const plan = sv::build_render_plan(def, output_size, index++, history);
            auto const outcome = sv::viewer_renderer::execute(
                *cmd, def, plan, resources, store, output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
            // Read back whatever reached the output, declined or not: a window shows a declined frame too.
            auto readback = sg::data_future<u32>(cmd->download.bytes_from_texture(output.raw()));
            if (outcome != sg::routine_outcome::executed)
                ++skipped;
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            sv_test::drive_ambient_work();

            auto const pixels_async = readback.data();
            auto const& pixels = co_await pixels_async;
            auto const now = cc::span<u32 const>(pixels.data(), pixels.size());
            stats.push_back(stats_of(now, previous));
            if (shown && stats.back().dark > worst_dark)
            {
                worst_dark = stats.back().dark;
                worst_at = f;
            }
            if (outcome == sg::routine_outcome::executed)
                executed.push_back(stats.back());
            shown = shown || outcome == sg::routine_outcome::executed;
            previous.clear();
            for (auto const px : now)
                previous.push_back(px);
        }

        // What the phase did, in one line: the darkest frame, how much the last frames still change, and the brightness drift.
        auto const tail = executed.size() >= 8 ? cc::span<frame_stats const>(executed).last_n(8)
                                               : cc::span<frame_stats const>(executed);
        auto tail_change = 0.0f;
        for (auto const& s : tail)
            tail_change += s.change / f32(tail.size());
        auto const first_mean = executed.empty() ? 0.0f : executed[executed.size() / 2].mean;
        auto const last_mean = executed.empty() ? 0.0f : executed.back().mean;
        CC_LOG_INFO("phase '{}': {} frames, {} declined; darkest {:.3f} at frame {}; last frames change {:.2f}; mean "
                    "{:.1f} -> {:.1f}",
                    p.name, stats.size(), skipped, worst_dark, worst_at, tail_change, first_mean, last_mean);

        CHECK(worst_dark < dark_ceiling).dump("phase", cc::string_view(p.name)).dump("darkest", worst_dark).dump("at", worst_at);
        if (!p.moving)
        {
            CHECK(tail_change < 4.0f).dump("phase", cc::string_view(p.name)).dump("change", tail_change);
            CHECK(tg::abs(last_mean - first_mean) < 0.1f * first_mean + 2.0f)
                .dump("phase", cc::string_view(p.name))
                .dump("from", first_mean)
                .dump("to", last_mean);
        }
    }
    co_await cc::async_settled(sv::background_work(ctx));
}
