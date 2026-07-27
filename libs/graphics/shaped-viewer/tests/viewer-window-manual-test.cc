#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/input.hh>
#include <shaped-rendering/window.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual

#include <chrono>
#include <cmath>
#include <variant>

// Interactive demo: a random cloud of flat-shaded PBR triangles, ray-traced into a view target and blitted
// into a window, with a simple fly-camera you drive yourself.
//
// Controls:
//   hold right mouse button + move mouse — look around
//   W / S — forward / back      A / D — strafe      E / Q — up / down      Shift — move faster
//   Esc — quit
//
// nx::config::manual keeps it out of the default sweep. Run it explicitly:
//   uv run dev.py test "sv - viewer window (manual)" --manual --timeout 0
// Prefers a hardware GPU, falls back to WARP; SKIPs if the device has no ray tracing or there is no window.

namespace
{
/// A minimal free-fly camera controller — the kind of thing an app would grow into a real class, kept here
/// in the test for now. Holds its own position + yaw/pitch, consumes sr input events, and writes an sv::camera.
struct fly_camera
{
    tg::pos3f position = tg::pos3f(0, 1.4f, -5.0f);
    float yaw = 0.0f;   // radians, around +Y; 0 looks toward +Z
    float pitch = 0.0f; // radians, around the camera's right axis

    float move_speed = 3.0f;    // units / second
    float look_speed = 0.0035f; // radians / pixel

    bool looking = false; // right mouse button held
    bool key_forward = false, key_back = false, key_left = false, key_right = false;
    bool key_up = false, key_down = false, key_fast = false;

    [[nodiscard]] tg::vec3f forward() const
    {
        auto const cp = std::cos(pitch);
        return tg::vec3f(std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp).normalized();
    }

    [[nodiscard]] tg::vec3f right() const { return tg::dual(tg::cross(tg::vec3f(0, 1, 0), forward())).normalized(); }

    void handle(sr::input_event const& e, sr::window& win)
    {
        if (auto const* const k = std::get_if<sr::key_event>(&e.payload))
        {
            auto const down = k->is_down;
            switch (k->scancode)
            {
            case sr::scancode::w:
                key_forward = down;
                break;
            case sr::scancode::s:
                key_back = down;
                break;
            case sr::scancode::a:
                key_left = down;
                break;
            case sr::scancode::d:
                key_right = down;
                break;
            case sr::scancode::e:
                key_up = down;
                break;
            case sr::scancode::q:
                key_down = down;
                break;
            case sr::scancode::left_shift:
            case sr::scancode::right_shift:
                key_fast = down;
                break;
            case sr::scancode::escape:
                if (down)
                    win.request_close();
                break;
            default:
                break;
            }
        }
        else if (auto const* const b = std::get_if<sr::mouse_button_event>(&e.payload))
        {
            if (b->button == sr::mouse_button::right)
            {
                looking = b->is_down;
                win.set_relative_mouse_mode(looking); // capture the cursor while looking
            }
        }
        else if (auto const* const m = std::get_if<sr::mouse_move_event>(&e.payload))
        {
            if (looking)
            {
                yaw += m->delta[0] * look_speed;
                pitch -= m->delta[1] * look_speed;
                auto const limit = 1.5f; // keep just shy of straight up/down
                pitch = pitch < -limit ? -limit : (pitch > limit ? limit : pitch);
            }
        }
    }

    void update(float dt)
    {
        auto velocity = tg::vec3f(0, 0, 0);
        auto const f = forward();
        auto const r = right();
        if (key_forward)
            velocity += f;
        if (key_back)
            velocity -= f;
        if (key_right)
            velocity += r;
        if (key_left)
            velocity -= r;
        if (key_up)
            velocity += tg::vec3f(0, 1, 0);
        if (key_down)
            velocity -= tg::vec3f(0, 1, 0);

        auto const speed = move_speed * (key_fast ? 3.0f : 1.0f);
        position = position + velocity * (speed * dt);
    }

    void apply(sv::camera& cam) const
    {
        auto const eye = tg::pos3d(position[0], position[1], position[2]);
        auto const f = forward();
        cam.position = eye;
        cam.orientation = sv::camera::look_rotation(eye, eye + tg::vec3d(f[0], f[1], f[2]));
    }
};
} // namespace

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
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(cloud.positions);
    auto const materials = resources.materials.acquire(cloud.materials);

    auto controller = fly_camera{};

    auto last = std::chrono::steady_clock::now();
    auto const start = last;
    constexpr auto max_duration = std::chrono::minutes(10);

    while (!win->is_close_requested())
    {
        wsys->poll_events();
        if (wsys->is_quit_requested())
            break;
        for (auto const& e : wsys->events())
            controller.handle(e, *win);

        auto const now = std::chrono::steady_clock::now();
        auto const dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (now - start > max_duration)
            break;
        if (win->is_minimized())
            continue;

        controller.update(dt);

        auto def = sv::viewer_definition{};
        {
            auto v = sv::view{};
            v.id = sv::view_id::from_string("main");
            v.size = tg::vec2i(win->width(), win->height());
            controller.apply(v.camera);
            v.items.push_back({.mesh = mesh, .materials = materials});
            v.area_lights.push_back({.emission = tg::vec3f(14.0f, 14.0f, 14.0f)}); // default overhead rect, facing down

            // A cool-blue SH sky: a bright ambient DC term plus a vertical gradient (brighter toward the zenith,
            // +y). The path tracer's miss shows it behind the cloud, and env NEE lights the cloud from it.
            v.background.sh[0] = tg::vec3f(1.6f, 2.2f, 3.2f);
            v.background.sh[1] = tg::vec3f(0.5f, 0.7f, 1.1f);

            def.views.push_back(cc::move(v));
        }

        auto rt = sc->acquire_backbuffer(); // auto-resizes to the window
        auto cmd = ctx.create_command_list();
        // The view_renderer path-traces the view and blits it into the back buffer, opening the scope itself.
        sv::view_renderer::execute(*cmd, def, resources, rt.cleared(tg::vec4f(0.02f, 0.02f, 0.03f, 1.0f)));
        ctx.submit_command_list_and_present(*sc, cc::move(cmd));
        ctx.advance_epoch(sc->buffer_count());
    }

    ctx.advance_epoch_and_wait_for_idle();
    CHECK(true); // manual visual test — reaching here means the frame loop ran and tore down cleanly
}
