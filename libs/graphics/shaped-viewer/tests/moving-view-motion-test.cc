#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The motion guide a moving view hands its temporal member, read out of a real frame of each renderer.
//
// motion-vector-test pins the formula through a probe; this pins what a view feeds it while the camera moves.
// That is the previous camera a frame is given, and the jitter its samples sit at, either of which a temporal member reads as the image wobbling.
// The camera steps sideways in front of a wall, so every pixel's motion is one number: the step times the focal length over the depth.

namespace
{
using namespace cc::primitive_defines;
} // namespace

ASYNC_INVOCABLE_TEST("sv - a view stepping sideways in front of a wall reports the step as every pixel's motion",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto resources = sv::gpu_resource_manager::create(ctx);

    // A wall at z = 0, far wider than anything the camera sees.
    tg::pos3f const corners[]
        = {tg::pos3f(-50, -50, 0), tg::pos3f(50, -50, 0), tg::pos3f(50, 50, 0), tg::pos3f(-50, 50, 0)};
    auto positions = cc::vector<tg::pos3f>();
    for (auto const i : {0, 1, 2, 0, 2, 3})
        positions.push_back(corners[i]);
    using binding = sv::material_attribute_binding;
    auto const item = resources.acquire_scene_item(
        sv::mesh{.name = "motion wall",
                 .geometry = sv::triangle_geometry::create_from_positions(positions),
                 .material = lib.acquire(sv::material::create(
                     "motion wall", openpbr, {binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f))}))});
    resources.wait_for_pending_uploads();

    constexpr auto size = tg::vec2i(64, 64);
    constexpr auto depth = 5.0;
    constexpr auto step = 0.01;
    auto const fov = tg::angle_d::make_from_degree(60.0);
    // The camera looks along +z, so a step toward +x is a step to its left.
    // The previous camera stood to the RIGHT, so the wall sat further left on its image, and this pixel minus that one is positive.
    auto const expected = step * (0.5 * f64(size[1]) / tg::tan(fov * 0.5)) / depth;

    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    struct contender
    {
        char const* name = "";
        sv::render_mode mode = sv::render_mode::hybrid;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred;
    };
    for (auto const c : {contender{.name = "hybrid deferred", .lighting = sv::hybrid_lighting::deferred},
                         contender{.name = "hybrid forward", .lighting = sv::hybrid_lighting::forward},
                         contender{.name = "megakernel", .mode = sv::render_mode::megakernel},
                         contender{.name = "wavefront", .mode = sv::render_mode::wavefront}})
    {
        auto def = sv::viewer_definition{};
        {
            auto v = sv::view_data{};
            v.id = sv::view_id::from_string("moving");
            auto& scene = sv::ensure_scene_3d(v);
            scene.items.push_back(item);
            scene.background = sv::background::uniform(tg::vec3f(0.5f, 0.5f, 0.5f));
            scene.settings.mode = c.mode;
            scene.settings.hybrid_lighting = c.lighting;
            scene.settings.reconstruct.denoiser = sr::denoise_method::automatic;
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

        // The camera the guide slot last recorded, which a layer updates only on a frame it traced.
        auto const last_traced_x = [&]() -> cc::optional<f32>
        {
            auto const* const state = store.peek_ptr(id);
            auto const* const slot
                = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::motion_guide(0)) : nullptr;
            if (slot == nullptr || !slot->has_last_camera)
                return {};
            return slot->last_camera.position[0];
        };

        // Frames past the first one that wrote a guide: the first has no previous camera, so it reports none.
        // Only a guide from a traced frame after a traced frame counts: a layer declines while a compile is pending,
        // which leaves the guide as it was, and the next traced frame's motion reaches back to the last traced camera.
        auto settled = -1;
        auto index = u64(0);
        auto was_traced = false;
        auto guide = cc::vector<tg::vec2f>();
        while (settled < 4)
        {
            auto& cam = def.views[0].camera;
            cam = sv::camera::looking_at(tg::pos3d(step * f64(index), 0, -depth), tg::pos3d(step * f64(index), 0, 0));
            cam.projection.aspect_ratio = 1.0;
            cam.projection.vertical_fov = fov;

            (void)ctx.routines.tick();
            auto cmd = ctx.create_command_list();
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            auto const plan = sv::build_render_plan(def, size, index++, history);
            auto const before = last_traced_x();
            (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                               output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
            auto const after = last_traced_x();
            auto const is_traced = after.has_value() && (!before.has_value() || after.value() != before.value());
            auto const follows_traced = is_traced && was_traced;
            was_traced = is_traced;
            auto const* const state = store.peek_ptr(id);
            auto const* const slot
                = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::motion_guide(0)) : nullptr;
            auto motion = sg::data_future<tg::vec2f>();
            if (slot != nullptr && slot->texture.raw() != nullptr)
                motion = sg::data_future<tg::vec2f>(cmd->download.bytes_from_texture(slot->texture.raw()));
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            sv_test::drive_ambient_work();
            // a frame that traced nothing was declined while a compile is pending: wait for it rather than spin
            if (!is_traced)
                co_await cc::async_settled(sv::background_work(ctx));
            // frames rather than seconds, since a loaded machine is slow without being wrong
            REQUIRE(index < 2000);
            if (!motion.is_valid())
            {
                // Only a layer a temporal member denoises writes the guide.
                if (index > 600)
                    SKIP("no temporal member on this machine, so no view writes a motion guide");
                continue;
            }

            auto const pixels_async = motion.data();
            auto const& pixels = co_await pixels_async;
            auto const center = pixels[isize(size[1] / 2) * isize(size[0]) + isize(size[0] / 2)];
            if (!follows_traced || (settled < 0 && center[0] == 0.0f && center[1] == 0.0f))
                continue;
            ++settled;
            guide = cc::vector<tg::vec2f>();
            for (auto const& p : pixels)
                guide.push_back(p);
        }

        auto worst = 0.0;
        for (auto const& m : guide)
            worst = cc::max(worst, cc::max(tg::abs(f64(m[0]) - expected), tg::abs(f64(m[1]))));
        auto const center = guide[isize(size[1] / 2) * isize(size[0]) + isize(size[0] / 2)];
        CC_LOG_INFO("{}: a step of {} at depth {} moves the center by ({}, {}) pixels, expected ({}, 0); the worst "
                    "pixel is off by {}",
                    c.name, step, depth, center[0], center[1], expected, worst);
        CHECK(worst < 0.02).context(c.name).dump("expected", expected).dump("center x", center[0]).dump("center y", center[1]);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}
