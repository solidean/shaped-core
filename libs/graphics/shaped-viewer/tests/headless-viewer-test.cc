#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
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

// A capture, end to end: the environment alone turns the loop above into one that writes an image and stops.
//
// What this really pins is that the FILE IS COMPLETE.
// The write stream is buffered and its destructor does not drain, so a missing flush ends every image on a 4096-byte
// boundary — and a JPEG truncated that way still decodes, flat-filling the tail from the last DC value.
// That looks exactly like a rendering artifact, which is a far more expensive thing to debug than a short file, so
// decoding the result back and checking its extent is the assertion that matters here.
TEST("sv - a capture writes a complete image and ends the loop", nx::config::main_thread)
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

    // The OS scratch directory, not the working directory: a test must not leave a file in whatever tree it ran from.
    auto const path = cc::format("{}/sv-capture-test.jpg", cc::temp_directory_path());
    auto const size = tg::vec2i(96, 64);

    // Scoped, so a REQUIRE below cannot leak capture mode into every test that runs after this one.
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, "96x64");
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "4"); // WARP traces in software
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "120");

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    auto frames = 0;
    auto front_applied = 0;
    auto front_first_frames = 0;
    auto side_applied = 0;

    for (auto f : sv::interactive(ctx, "sv-test/capture"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 6.0});

        // Both are declared every frame, and only the one named by the environment ever runs.
        f.register_capture("front",
                           [&](sv::capture_context const& c)
                           {
                               ++front_applied;
                               front_first_frames += c.first_frame ? 1 : 0;
                               CHECK(c.name == "front");
                               CHECK(c.size == size);
                           });
        f.register_capture("side", [&](sv::capture_context const&) { ++side_applied; });

        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_light({.center = tg::pos3f(0, 1.9f, 0),
                         .half_extent_u = tg::vec3f(0.4f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 0.4f),
                         .emission = tg::vec3f(12, 12, 12)});

        ++frames;
        REQUIRE(frames < 400); // the capture ends the loop itself; this only stops a hang from becoming a timeout
    }

    // Read it back with a real decoder rather than checking that the file is non-empty: a truncated image is
    // non-empty, and that is the whole failure being guarded against.
    // The named capture ran on every frame, and reported its first exactly once — which is what a callback doing
    // one-shot setup relies on.
    CHECK(front_applied == frames);
    CHECK(front_first_frames == 1);

    // A capture nobody asked for is inert.
    // Registering one costs an example nothing on a run taking a different shot, and nothing at all on an interactive run.
    CHECK(side_applied == 0);

    // Scoped, because the adapter holds the file open and Windows will not remove one that is.
    {
        auto reread = cc::file_read_stream_adapter::open(path);
        REQUIRE(reread.has_value());
        cc::read_stream in = reread.value();

        auto const decoded = babel::image::read(in);
        REQUIRE(decoded.has_value());
        CHECK(decoded.value().width == size[0]);
        CHECK(decoded.value().height == size[1]);
    }
    cc::remove_file(path);
}

// A capture asked for by a name nothing registers must fail, and must fail WITHOUT writing anything.
//
// Nothing discovers capture names any more — a `.capture.json` beside the example declares them — so this is the only
// thing standing between a renamed callback and a plausible, wrong reference image: the default view, written under
// the old name's filename, refreshed into the repository by a sweep that reported success.
TEST("sv - a capture nothing registered fails without writing", nx::config::main_thread)
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

    auto const path = cc::format("{}/sv-capture-missing.jpg", cc::temp_directory_path());
    cc::remove_file(path); // a leftover from an earlier run would make the check below vacuous

    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, "64x48");
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "no-such-capture");
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "120");

    auto frames = 0;
    for (auto f : sv::interactive(ctx, "sv-test/capture-missing"))
    {
        f.window().view().add_scene();
        f.register_capture("front", [](sv::capture_context const&) {});

        ++frames;
        REQUIRE(frames < 200); // it should stop on the first frame; this only keeps a hang from becoming a timeout
    }

    // The first frame is enough to know: registration happens while a frame is authored.
    CHECK(frames == 1);

    // And nothing was written.
    // A file here would be the default view wearing the requested name.
    CHECK(cc::file_read_stream_adapter::open(path).has_error());
}
