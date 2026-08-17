#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/blit_routine.hh>
#include <shaped-rendering/input.hh>
#include <shaped-rendering/window.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/scalar/angle.hh>

#include <chrono>

using namespace cc::primitive_defines;

// Interactive demo: the path-traced Cornell box (global illumination via next-event estimation + diffuse
// bounces) driven straight through sv::pathtrace_routine and blitted into a window, with a free-fly camera.
//
// Temporal accumulation with reprojection: each frame reprojects the previous one's image through the camera it was
// traced from and blends into it per pixel, so a still camera converges to a clean image within a second or two.
// A camera move no longer restarts the average — it carries, and only the pixels failing the disocclusion test start
// over — so moving stays far cleaner than a from-scratch trace would be.
// Only a few samples are traced per frame, keeping it responsive while moving.
//
// Controls:
//   hold right mouse button + move mouse — look around
//   W / S — forward / back      A / D — strafe      E / Q — up / down      Shift — move faster
//   Tab — toggle the accumulation debug view: red where a pixel kept no history, green as its count climbs.
//         Red that never turns green is the history being rejected, which is what permanent noise looks like.
//   Esc — quit
//
// nx::config::manual keeps it out of the default sweep.
// Run it explicitly:
//   uv run dev.py test "sv - path-traced window (manual)" --manual --timeout 0
// Prefers a hardware GPU, falls back to WARP (WARP path tracing is slow); SKIPs without RT or a window.

namespace
{
/// A minimal free-fly camera controller — position + yaw/pitch, consumes sr input, writes an sv::camera.
struct fly_camera
{
    tg::pos3f position = tg::pos3f(0, 0, -3.2f);
    tg::angle_f yaw;   // around +Y; 0 looks toward +Z (into the open front of the box)
    tg::angle_f pitch; // around the camera's right axis

    float move_speed = 2.0f;                                          // units / second
    tg::angle_f look_speed = tg::angle_f::make_from_radians(0.0035f); // per pixel of mouse motion

    bool looking = false;    // right mouse button held
    bool debug_view = false; // Tab: show carried sample counts instead of the image
    bool key_forward = false, key_back = false, key_left = false, key_right = false;
    bool key_up = false, key_down = false, key_fast = false;

    [[nodiscard]] tg::vec3f forward() const
    {
        auto const cp = pitch.cos();
        return tg::vec3f(yaw.sin() * cp, pitch.sin(), yaw.cos() * cp).normalized();
    }

    [[nodiscard]] tg::vec3f right() const { return tg::dual(tg::cross(tg::vec3f(0, 1, 0), forward())).normalized(); }

    void handle(sr::input_event const& e, sr::window& win)
    {
        e.payload.visit(
            [&](sr::key_event const& k)
            {
                auto const down = k.is_down;
                switch (k.scancode)
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
                case sr::scancode::tab:
                    if (down)
                        debug_view = !debug_view;
                    break;
                case sr::scancode::escape:
                    if (down)
                        win.request_close();
                    break;
                default:
                    break;
                }
            },
            [&](sr::mouse_button_event const& b)
            {
                if (b.button == sr::mouse_button::right)
                {
                    looking = b.is_down;
                    win.set_relative_mouse_mode(looking); // capture the cursor while looking
                }
            },
            [&](sr::mouse_move_event const& m)
            {
                if (!looking)
                    return;

                yaw += look_speed * float(m.delta[0]);
                pitch -= look_speed * float(m.delta[1]);
                auto const limit = tg::angle_f::make_from_radians(1.5f); // keep just shy of straight up/down
                pitch = pitch < -limit ? -limit : (pitch > limit ? limit : pitch);
            },
            [](sr::mouse_wheel_event const&) {}, [](sr::text_event const&) {});
    }

    /// Returns whether the camera moved this step, which is what decides how long a reprojected sample lives.
    [[nodiscard]] bool update(float dt)
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

        auto const moving = key_forward || key_back || key_left || key_right || key_up || key_down;
        if (moving)
        {
            auto const speed = move_speed * (key_fast ? 3.0f : 1.0f);
            position = position + velocity * (speed * dt);
        }
        return moving || looking; // a look this frame counts as motion too
    }

    void apply(sv::camera& cam) const
    {
        auto const eye = tg::pos3d(position[0], position[1], position[2]);
        auto const f = forward();
        cam.position = eye;
        cam.orientation = sv::camera::look_rotation(eye, eye + tg::vec3d(f[0], f[1], f[2]));
        cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    }
};
} // namespace

TEST("sv - path-traced window (manual)", nx::config::manual)
{
    auto wsys_r = sr::window_system::try_create();
    if (wsys_r.has_error())
        SKIP("no window backend (SDL3 not built) — cannot open a window");
    auto const wsys = cc::move(wsys_r.value());

    auto win_r
        = wsys->try_create_window({.title = "shaped-viewer — path-traced Cornell box", .width = 1000, .height = 1000});
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
        SKIP("no DXC compiler to build the path-tracing shaders");

    auto sc_r = ctx.try_create_swapchain(
        {.native_window_handle = win->native_window_handle(), .buffer_count = 3, .format = sg::pixel_format::bgra8_unorm});
    if (sc_r.has_error())
        SKIP("could not create a swapchain for the window");
    sg::swapchain_handle const sc = sc_r.value();

    // Build the Cornell box once; only the camera moves.
    auto const box = sv_test::make_cornell_box();
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(box.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(box.materials));
    auto const* const mesh_rec = resources.meshes.get_ptr(mesh);
    auto const* const mat_rec = resources.materials.get_ptr(materials);
    CC_ASSERT(mesh_rec != nullptr && mat_rec != nullptr, "cornell box resources failed to resolve");

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0});

    auto controller = fly_camera{};

    // A ping-pong pair, not one texture: the raygen reprojects, so it reads at a different pixel than it writes and
    // cannot do that in place.
    // `write` names the half this frame fills; the other is last frame's, read-only.
    // The G-buffer needs the same treatment, since the disocclusion test reads the previous frame's geometry.
    sg::texture_2d color[2] = {};
    sg::texture_2d gbuffer[2] = {};
    auto write = 0;

    auto target_size = tg::vec2i(0, 0);
    auto accum = u32(0);

    // What the reprojection maps through, and whether there is a frame to map at all.
    auto prev_camera = sv::camera_gpu{};
    auto has_history = false;

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

        auto const moved = controller.update(dt);

        // (Re)create both pairs on a size change.
        // Nothing survives a resize to reproject, so this is the one thing that still restarts the whole image — a
        // camera move no longer does.
        auto const size = tg::vec2i(win->width(), win->height());
        if (size != target_size)
        {
            auto const desc = sg::texture_2d_description{
                .format = sg::pixel_format::rgba16_float,
                .width = size[0],
                .height = size[1],
                .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture};

            for (auto i = 0; i < 2; ++i)
            {
                color[i] = ctx.persistent.create_texture_2d(desc);
                gbuffer[i] = ctx.persistent.create_texture_2d(desc);
            }

            target_size = size;
            accum = 0;
            has_history = false;
        }

        auto cam = sv::camera{};
        controller.apply(cam);
        cam.projection.aspect_ratio = double(size[0]) / double(size[1] > 0 ? size[1] : 1);

        auto fc = sv::pt_frame_constants_gpu{};
        fc.camera = sv::camera_gpu::from(cam);
        // the box light is an axis-aligned XZ rect, emitting straight down
        fc.light = {.center = box.light.center,
                    .u = tg::vec3f(box.light.half_x, 0, 0),
                    .v = tg::vec3f(0, 0, box.light.half_z),
                    .emission = box.light.emission,
                    .normal = tg::vec3f(0, -1, 0)};
        fc.samples_per_pixel = 2; // low per-frame count — accumulation does the heavy lifting when still
        fc.max_bounces = 5;
        fc.accum_frame = accum;
        fc.seed = accum + 1;
        fc.mesh_is_indexed = mesh_rec->is_indexed;

        fc.prev_camera = prev_camera;
        fc.has_history = has_history;

        // Uncapped while still, so a parked camera converges to the exact mean.
        // Capped once moving, so a sample dragged along by reprojection ages out instead of smearing.
        fc.history_max_frames = moved ? 32u : u32(-1);
        fc.debug_view = controller.debug_view ? 1u : 0u;

        // Trace this frame's samples into the persistent target (blending in place when accum_frame > 0).
        {
            auto trace_cmd = ctx.create_command_list();
            auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            trace_cmd->upload.pod_to_buffer(frame, fc);

            // Closed Cornell box: no ray escapes, so the environment probe stays dark — bind an all-zero one.
            auto const background = ctx.transient.create_buffer<sv::background_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            trace_cmd->upload.pod_to_buffer(background, sv::background_gpu::from(sv::background{}));

            auto const read = 1 - write;
            sv::pathtrace_routine::execute(*trace_cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = color[write],
                                                        .gbuffer = gbuffer[write],
                                                        .history_color = color[read],
                                                        .history_gbuffer = gbuffer[read],
                                                        .materials = mat_rec->materials,
                                                        .vertices = mesh_rec->vertices,
                                                        .indices = mesh_rec->indices});
            ctx.submit_command_list(cc::move(trace_cmd));
        }

        // Blit the accumulated image into the back buffer and present.
        auto rt = sc->acquire_backbuffer(); // auto-resizes to the window
        auto cmd = ctx.create_command_list();
        {
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.0f, 0.0f, 0.0f, 1.0f))}});
            sr::blit_routine::execute(pass, color[write]); // the half this frame just wrote
        }
        ctx.submit_command_list_and_present(*sc, cc::move(cmd));
        ctx.advance_epoch(sc->buffer_count());

        // What this frame wrote becomes next frame's history — both halves of the pair, and the camera behind them.
        prev_camera = fc.camera;
        has_history = true;
        write = 1 - write;

        if (accum < 4096) // cap so the running mean's weight stays well within half-float precision
            ++accum;
    }

    ctx.advance_epoch_and_wait_for_idle();
    CHECK(true); // manual visual test — reaching here means the frame loop ran and tore down cleanly
}
