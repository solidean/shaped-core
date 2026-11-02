#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// An accumulated image is antialiased: each frame samples its pixels somewhere else inside them, so a pixel an edge
// crosses converges to the share of it the surface covers.
//
// A glowing half-plane with a slanted edge, over black, under no light, is the case whose answer is plain.
// Every pixel the edge crosses ends between black and the glow, and a renderer that samples every frame at the
// pixels' centers leaves them all at one or the other however long it accumulates.

namespace
{
using namespace cc::primitive_defines;

/// The glow's radiance, which a pixel the half-plane covers converges to.
constexpr f32 glow = 2.0f;

[[nodiscard]] f32 luminance(tg::vec4f const& p)
{
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - an accumulated image is antialiased: a pixel an edge crosses converges to its coverage",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    auto& lib = sv_test::shared_material_library();
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const glowing = lib.acquire(
        sv::material::create("jitter glow", pbr,
                             {sv::material_attribute_binding::of("base_color", tg::vec3f(0, 0, 0)),
                              sv::material_attribute_binding::of("emissive", tg::vec3f(glow, glow, glow))}));
    // A half-plane facing the camera, its edge running through the middle of the view at a slant, so it crosses
    // each row at a different place inside its pixels.
    auto const e = 20.0f;
    auto const slant = 0.3f;
    auto const half_plane = sv::mesh{.name = "jitter half plane",
                                     .geometry = sv::triangle_geometry::create_from_positions(cc::vector<tg::pos3f>{
                                         tg::pos3f(-slant * e, -e, 0), tg::pos3f(slant * e, e, 0), tg::pos3f(e, e, 0),
                                         tg::pos3f(-slant * e, -e, 0), tg::pos3f(e, e, 0), tg::pos3f(e, -e, 0)}),
                                     .material = glowing};

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(half_plane);
    resources.wait_for_pending_uploads();

    constexpr auto size = tg::vec2i(64, 64);
    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto index = u64(0);

    for (auto const mode : {sv::render_mode::hybrid, sv::render_mode::wavefront, sv::render_mode::megakernel})
    {
        auto def = sv::viewer_definition{};
        {
            auto v = sv::view_data{};
            v.id = sv::view_id::from_string("jittered");
            v.camera = sv::camera::looking_at(tg::pos3d(0, 0, -3), tg::pos3d(0, 0, 0));
            v.camera.projection.aspect_ratio = 1.0;
            auto& scene = sv::ensure_scene_3d(v);
            scene.items.push_back(item);
            scene.background = sv::background::uniform(tg::vec3f(0, 0, 0));
            scene.fallback_light = cc::nullopt;
            scene.settings.mode = mode;
            scene.settings.reconstruct.denoiser = sr::denoise_method::none;
            def.views.push_back(cc::move(v));

            auto const root_node = def.nodes.add_container(sv::invalid_node);
            auto leaf = sv::layout_leaf{};
            leaf.views.push_back(sv::view_index(0));
            def.nodes.add_leaf(root_node, cc::move(leaf));
            auto root = sv::view_data{};
            root.id = sv::view_id::from_string("root");
            root.layers.push_back(
                {.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
            def.root_view = sv::view_index(def.views.size());
            def.views.push_back(cc::move(root));
        }
        auto store = sv::view_store{};
        auto const history = sv::view_history{};
        auto const id = def.views[0].id;

        // Until the mean holds enough samples for a pixel's coverage to show, and every compile has landed.
        auto pixels = cc::vector<tg::vec4f>();
        auto const start = cc::current_time_steady_secs();
        while (true)
        {
            (void)ctx.routines.tick();
            auto cmd = ctx.create_command_list();
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            auto const plan = sv::build_render_plan(def, size, index++, history);
            (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                               output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
            auto const* const state = store.peek_ptr(id);
            auto const* const slot
                = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
            auto accumulated = sg::data_future<tg::vec4f>();
            if (slot != nullptr && slot->texture.raw() != nullptr)
                accumulated = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot->texture.raw()));
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            sv_test::drive_ambient_work();
            REQUIRE(cc::current_time_steady_secs() - start < 60.0);
            if (store.min_accumulated_samples(id) < 64 || !accumulated.is_valid())
                continue;
            auto const& data = co_await accumulated.data();
            pixels.clear();
            for (auto const& px : data)
                pixels.push_back(px);
            break;
        }
        REQUIRE(pixels.size() == isize(size[0]) * size[1]);

        // In every row the edge crosses one or two pixels, and those should hold part of the glow.
        // A row whose edge runs within a hair of a pixel's border covers it all but a sliver, so not every row shows one;
        // sampling every frame at the centers shows none at all.
        auto partial = 0;
        auto rows_with_partial = 0;
        for (auto y = 4; y < size[1] - 4; ++y)
        {
            auto in_row = 0;
            for (auto x = 0; x < size[0]; ++x)
            {
                auto const l = luminance(pixels[y * size[0] + x]);
                if (l > 0.02f * glow && l < 0.98f * glow)
                    ++in_row;
            }
            partial += in_row;
            if (in_row > 0)
                ++rows_with_partial;
        }
        auto const rows = size[1] - 8;
        CC_LOG_INFO("mode {}: {} of {} rows hold a pixel between black and the glow, {} such pixels", int(mode),
                    rows_with_partial, rows, partial);
        CHECK(rows_with_partial >= rows * 4 / 5)
            .dump("mode", int(mode))
            .dump("rows with a partial pixel", rows_with_partial)
            .dump("rows", rows);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}
