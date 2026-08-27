#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// The headless viewer: a full frame loop with no window system, no window and no swapchain.
//
// This is what a capture run drives, so what it pins is that the authoring surface cannot tell the difference —
// same frame, same handles, same `viewport_size` — while nothing ever touches a display.
// On the main thread because the path tracer's shader compiles run inline through `try_async_blocking_get`, which
// does not complete from inside a pool worker (same reason as `pathtraced-view-test`).
TEST("sv - headless viewer runs a frame loop with no window", nx::config::main_thread)
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
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

    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the path-tracing shaders");

    auto const size = tg::vec2i(160, 120); // small: WARP traces every pixel in software
    auto v_r = sv::viewer::try_create(ctx, "sv-test/headless", {.width = size[0], .height = size[1], .headless = true});
    REQUIRE(v_r.has_value());
    auto viewer = cc::move(v_r.value());

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    // Built once, outside the loop, exactly as an example builds its mesh — that is what pins the buffers and hashes
    // them, so placing it every frame uploads nothing after the first.
    auto frames_drawn = 0;
    auto accumulated = u32(0);
    auto pending_at_end = isize(-1);

    for (auto f : viewer.frames())
    {
        CHECK(f.viewport_size() == size);

        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 6.0});

        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_light({.center = tg::pos3f(0, 1.9f, 0),
                         .half_extent_u = tg::vec3f(0.4f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 0.4f),
                         .emission = tg::vec3f(12, 12, 12)});

        accumulated = view.accumulated_frames();
        pending_at_end = f.pending_resource_work();

        // A headless loop is ended by the body alone: nothing polls, so there is no close button and no quit.
        if (++frames_drawn >= 8)
            viewer.request_close();
    }

    CHECK(frames_drawn == 8);

    // The accumulator is read while authoring, so it reports what the PREVIOUS frame integrated — seven, not eight.
    // What matters is that it climbed at all: a trace that never dispatched leaves it at zero forever.
    CHECK(accumulated > 0);
    CHECK(pending_at_end == 0);
}
