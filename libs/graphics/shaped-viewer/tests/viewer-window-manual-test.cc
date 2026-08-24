#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/input.hh>
#include <shaped-rendering/window.hh>
#include <shaped-viewer/all.hh>

#include <chrono>

using namespace cc::primitive_defines;

// Interactive demo: a random cloud of flat-shaded PBR triangles, ray-traced into a view target and blitted into a window.
//
// It drives the retained path by hand — its own window, swapchain and frame loop — so it is also where sv::orbit_camera_controller
// is exercised outside the immediate-mode viewer, on a caller that owns its own event pump.
// The view accumulates while the camera is still, and restarts whenever the controller reports motion.
//
// Controls:
//   left mouse drag — orbit      middle mouse drag — pan      wheel — zoom
//
// nx::config::manual keeps it out of the default sweep.
// Run it explicitly:
//   uv run dev.py test "sv - viewer window (manual)" --manual --timeout 0
// Prefers a hardware GPU, falls back to WARP; SKIPs if the device has no ray tracing or there is no window.

TEST("sv - viewer window (manual)", nx::config::manual)
{
    auto wsys_r = sr::window_system::try_create();
    if (wsys_r.has_error())
        SKIP("no window backend (SDL3 not built) — cannot open a window");
    auto const wsys = cc::move(wsys_r.value());

    auto win_r = wsys->try_create_window({.title = "shaped-viewer — ray-traced PBR cloud", .width = 1440, .height = 900});
    if (win_r.has_error())
        SKIP("could not create a window");
    auto const win = cc::move(win_r.value());

    auto ctx_r = sg::create_dx12_context({});
    if (ctx_r.has_error())
        ctx_r = sg::create_dx12_context({.use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the ray-tracing shaders");

    auto sc_r = ctx.try_create_swapchain(
        {.native_window_handle = win->native_window_handle(), .buffer_count = 3, .format = sg::pixel_format::bgra8_unorm});
    if (sc_r.has_error())
        SKIP("could not create a swapchain for the window");
    sg::swapchain_handle const sc = sc_r.value();

    // Build the scene once; only the camera moves.
    auto const cloud = sv_test::make_triangle_cloud(96);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));

    auto controller = sv::orbit_camera_controller{};
    controller.orbit = {.target = tg::pos3d(0, 1, 0), .distance = 6.0};

    // What the views keep across frames, held here because this loop is the frame — a viewer would own it instead.
    auto store = sv::view_store{};

    auto const start = std::chrono::steady_clock::now();
    constexpr auto max_duration = std::chrono::minutes(10);

    auto frame_index = u64(0);
    while (!win->is_close_requested())
    {
        wsys->poll_events();
        if (wsys->is_quit_requested())
            break;
        // The controller is time-free, so there is no dt to integrate — each event carries the motion it caused.
        for (auto const& e : wsys->events())
            (void)controller.handle(e);

        if (std::chrono::steady_clock::now() - start > max_duration)
            break;
        if (win->is_minimized())
            continue;

        auto def = sv::viewer_definition{};
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("main");
        v.resolution = tg::vec2i(win->width(), win->height());
        // The camera is the only thing that changes, and the trace notices on its own — no restart to signal here.
        v.camera = controller.camera();
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        // an overhead rect facing down (cross(+x, +z) is -y)
        sv::ensure_scene_3d(v).area_lights.push_back({.center = tg::pos3f(0, 3, 0),
                                                      .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                                      .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                                      .emission = tg::vec3f(14.0f, 14.0f, 14.0f)});

        // A cool-blue SH sky, brighter toward the zenith (+y).
        // The path tracer's miss shows it behind the cloud, and env NEE lights the cloud from it.
        sv::ensure_scene_3d(v).background
            = sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f));

        def.views.push_back(cc::move(v));

        // One view filling the window: a root whose single layout leaf names it.
        auto const root_node = def.nodes.add_container(sv::invalid_node);
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(0));
        def.nodes.add_leaf(root_node, cc::move(leaf));

        auto root = sv::view_data{};
        root.id = sv::view_id::from_string("root");
        root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
        def.root_view = sv::view_index(def.views.size());
        def.views.push_back(cc::move(root));

        auto rt = sc->acquire_backbuffer(); // auto-resizes to the window
        auto cmd = ctx.create_command_list();
        // Both once per frame, before any view resolves its ids or reaches for its accumulator.
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));

        // The whole frame in one call: flatten it into a plan, trace every view, then composite up to the back buffer.
        auto const plan = sv::build_render_plan(def, tg::vec2i(win->width(), win->height()), frame_index, {});
        sv::viewer_renderer::execute(*cmd, def, plan, resources, store, rt.cleared(tg::vec4f(0.02f, 0.02f, 0.03f, 1.0f)));
        ctx.submit_command_list_and_present(*sc, cc::move(cmd));
        ctx.advance_epoch(sc->buffer_count());
        ++frame_index;
    }

    ctx.advance_epoch_and_wait_for_idle();
    CHECK(true); // manual visual test — reaching here means the frame loop ran and tore down cleanly
}
