// A rotating cube on metal, in a window, driven by the mouse.
//
// It is `graphics/rotating-cube` for the one backend that example cannot reach.
// The geometry, the camera and the lighting are the same; what differs is where the shaders come from.
// rotating-cube acquires them through slib, which compiles HLSL at runtime — and slib has no metal language, so on a
// Mac that example builds against nothing.
// Here the metallib is compiled from `shaders/cube.metal` ahead of time and embedded, the way every metal tier-2
// fixture does it.
//
// **This is a stopgap with a recorded replacement.** libs/graphics/shaped-graphics/docs/TODO.md carries both halves:
// metal as an `SC_EXAMPLE_BACKEND` value, and a shader package that can hand a metal context its bytecode.
// Once the second lands, rotating-cube grows a metal arm and this example has no reason to exist.
//
// Under `--capture` there is no window and no swapchain: the frame goes into a texture and is written out, which is
// how the example is verified on a machine with no display.

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/window.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>

#include "shaders/cube.metallib.h"

using namespace cc::primitive_defines;

namespace
{
/// One corner of the cube.
///
/// The layout this struct has is the layout `cube_vertex_layout()` below declares, and the two must agree — there is
/// no generated mirror here to hold them together, which is exactly what the shader package would buy.
struct cube_vertex
{
    tg::pos3f position;
    tg::vec3f normal;
    tg::vec3f color;
};

constexpr int cube_vertex_count = 24; // four per face: a shared corner carries three different normals
constexpr int cube_index_count = 36;

/// The 64-byte inline-constants block `cube.metal` reads at [[buffer(4)]].
struct cube_constants
{
    tg::mat4f view_projection;
};

static_assert(sizeof(cube_constants) == 64, "the inline-constants block is one mat4");

/// Slot 0 carries all three attributes, and an attribute's `[[attribute(n)]]` index is its position in this list.
[[nodiscard]] sg::vertex_input_layout cube_vertex_layout()
{
    auto layout = sg::vertex_input_layout{};
    layout.slots.push_back({.stride = isize(sizeof(cube_vertex))});
    layout.attributes.push_back({.semantic = "POSITION",
                                 .format = sg::vertex_attribute_format::vec3f,
                                 .offset = isize(offsetof(cube_vertex, position)),
                                 .slot = 0});
    layout.attributes.push_back({.semantic = "NORMAL",
                                 .format = sg::vertex_attribute_format::vec3f,
                                 .offset = isize(offsetof(cube_vertex, normal)),
                                 .slot = 0});
    layout.attributes.push_back({.semantic = "COLOR",
                                 .format = sg::vertex_attribute_format::vec3f,
                                 .offset = isize(offsetof(cube_vertex, color)),
                                 .slot = 0});
    return layout;
}

/// One stage of the embedded library.
/// Every stage carries the whole blob: a metallib holds both entry points, and the format is what says so.
[[nodiscard]] sg::compiled_shader cube_stage(sg::shader_stage stage, cc::string entry)
{
    auto shader = sg::compiled_shader{};
    shader.stage = stage;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = cc::move(entry);

    auto blob = cc::pinned_data<byte>::create_uninitialized(isize(sizeof(metal_cube::cube_metallib)));
    cc::memcpy(blob.data(), metal_cube::cube_metallib, sizeof(metal_cube::cube_metallib));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    return shader;
}

// TODO(typed-geometry): perspective / look_at belong in tg's transform module — see its docs/modules/transform.md.
// Copied from examples/graphics/rotating-cube/rotating_cube.cc, which carries the same note and the same reason:
// until tg pins a handedness and a depth range there is nothing to call.
// Left-handed, z into [0, 1]; tg::mat is COLUMN-major and subscripts m[col, row].

/// The classic 3-vector cross product.
/// tg's `cross` is the wedge and returns a bivector, so the vector form is its Hodge dual.
[[nodiscard]] tg::vec3f cross3(tg::vec3f a, tg::vec3f b) { return tg::dual(tg::cross(a, b)); }

[[nodiscard]] tg::mat4f perspective(tg::angle_f vertical_fov, float aspect, float z_near, float z_far)
{
    auto const t = 1.0f / tg::tan(vertical_fov / 2.0f);

    auto m = tg::mat4f::zero;
    m[0, 0] = t / aspect;
    m[1, 1] = t;
    m[2, 2] = z_far / (z_far - z_near);
    m[3, 2] = -z_near * z_far / (z_far - z_near);
    m[2, 3] = 1.0f;
    return m;
}

[[nodiscard]] tg::mat4f look_at(tg::pos3f eye, tg::pos3f target, tg::vec3f up)
{
    auto const f = tg::normalize(target - eye);
    auto const r = tg::normalize(cross3(up, f));
    auto const u = cross3(f, r);
    auto const e = eye - tg::pos3f::zero;

    auto m = tg::mat4f::identity;
    for (auto i = 0; i < 3; ++i)
    {
        m[i, 0] = r[i];
        m[i, 1] = u[i];
        m[i, 2] = f[i];
    }
    m[3, 0] = -tg::dot(r, e);
    m[3, 1] = -tg::dot(u, e);
    m[3, 2] = -tg::dot(f, e);
    return m;
}

/// The camera: it orbits a fixed target, and that is all the state this example keeps.
struct orbit_camera
{
    float distance = 2.9f;
    tg::angle_f yaw = tg::angle_f::make_from_degree(35.0f);
    tg::angle_f pitch = tg::angle_f::make_from_degree(24.0f);

    [[nodiscard]] tg::pos3f eye() const
    {
        auto const cp = tg::cos(pitch);
        return tg::pos3f::zero + tg::vec3f(cp * tg::sin(yaw), tg::sin(pitch), cp * tg::cos(yaw)) * distance;
    }

    [[nodiscard]] tg::mat4f view_projection(float aspect) const
    {
        auto const view = look_at(this->eye(), tg::pos3f::zero, tg::vec3f(0, 1, 0));
        return perspective(tg::angle_f::make_from_degree(45.0f), aspect, 0.1f, 100.0f) * view;
    }

    /// Clamped just short of the poles, where the up vector and the view direction would be parallel.
    void orbit(tg::vec2f drag)
    {
        auto const limit = tg::angle_f::make_from_degree(89.0f);
        yaw = yaw + tg::angle_f::make_from_degree(drag[0] * 0.35f);
        pitch = cc::clamp(pitch + tg::angle_f::make_from_degree(drag[1] * 0.35f), -limit, limit);
    }

    void zoom(float ticks) { distance = cc::clamp(distance * tg::pow(1.12f, -ticks), 1.6f, 40.0f); }
};

/// The unit cube, expanded so every face carries its own normal and its own color.
[[nodiscard]] cc::array<cube_vertex> build_cube_mesh()
{
    tg::vec3f const normals[] = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1),  tg::vec3f(-1, 0, 0),
                                 tg::vec3f(1, 0, 0),  tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};
    tg::vec3f const colors[] = {tg::vec3f(0.90f, 0.32f, 0.30f), tg::vec3f(0.30f, 0.62f, 0.90f),
                                tg::vec3f(0.42f, 0.80f, 0.42f), tg::vec3f(0.94f, 0.74f, 0.28f),
                                tg::vec3f(0.66f, 0.44f, 0.88f), tg::vec3f(0.94f, 0.94f, 0.92f)};

    auto out = cc::array<cube_vertex>::create_defaulted(cube_vertex_count);
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = normals[face];
        // Two in-plane axes, picked so the winding stays consistent across all six faces.
        auto const u = tg::vec3f(n[1], n[2], n[0]);
        auto const v = cross3(n, u);

        for (auto corner = 0; corner < 4; ++corner)
        {
            auto const su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            auto const sv = (corner >= 2) ? 1.0f : -1.0f;
            out[face * 4 + corner]
                = {.position = tg::pos3f::zero + (n + u * su + v * sv) * 0.5f, .normal = n, .color = colors[face]};
        }
    }
    return out;
}

[[nodiscard]] cc::array<u16> build_cube_indices()
{
    auto out = cc::array<u16>::create_defaulted(cube_index_count);
    for (auto face = 0; face < 6; ++face)
    {
        auto const base = u16(face * 4);

        // Reversed relative to the corner order above: the (u, v, n) basis is right-handed while the projection is
        // left-handed, so a face wound counter-clockwise there reaches the screen clockwise.
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(base + quad[i]);
    }
    return out;
}

[[nodiscard]] double now_seconds() { return cc::current_time_steady_secs(); }
} // namespace

ASYNC_EXAMPLE("shaped-graphics/metal-cube")
{
    // Read first: it decides whether there is a display in the picture at all.
    auto const capture = sr::capture_request::from_environment();
    if (capture.active && !capture.name.empty())
    {
        cc::eprintln("this example offers no named capture, so it cannot take {}", capture.name);
        co_return;
    }

    auto const created = sg::create_metal_context({});
    if (created.has_error())
    {
        cc::eprintln("no metal device: {}", created.error().to_string());
        co_return;
    }
    auto const ctx = created.value();

    // What write_capture_image_async reads back, and what a swapchain wants.
    auto const color_format = sg::pixel_format::bgra8_unorm;
    auto const depth_format = sg::pixel_format::depth32_float;
    auto size = capture.active ? capture.size : tg::vec2i(1280, 720);

    auto const vertices = ctx->persistent.create_buffer_from_data(build_cube_mesh(), sg::buffer_usage::vertex_buffer);
    auto const index_buffer = ctx->persistent.create_buffer_from_data(build_cube_indices(), sg::buffer_usage::index_buffer);

    auto const constants = sg::binding{.space = 0,
                                       .index = 0,
                                       .count = 1,
                                       .type = sg::binding_type::constants_buffer,
                                       .block_size = isize(sizeof(cube_constants))};

    auto const built = ctx->cached.acquire_raster_pipeline(
        {.layout = ctx->cached.acquire_pipeline_layout({.inline_constants = constants}),
         .vertex_shader = cube_stage(sg::shader_stage::vertex, "main_vs"),
         .fragment_shader = cube_stage(sg::shader_stage::fragment, "main_ps"),
         .vertex_input = cube_vertex_layout(),
         .rasterization = {.cull = sg::cull_mode::back},
         // Both default to OFF, and solid geometry needs both — a cube drawn without them shows whichever face
         // happened to be recorded last.
         .depth_stencil = {.depth_test = true, .depth_write = true},
         .color_targets = {{.format = color_format}},
         .depth_stencil_format = depth_format});
    co_await cc::async_settled(built);

    auto const* const pipeline_ptr = built->try_value();
    if (pipeline_ptr == nullptr)
    {
        auto const* const error = built->try_error();
        cc::eprintln("the raster pipeline did not build: {}",
                     error != nullptr ? error->underlying().to_string() : cc::string("the build never ran"));
        co_return;
    }
    auto const pipeline = *pipeline_ptr;

    // A window, or a texture to render into — the frame below does not care which.
    cc::unique_ptr<sr::window_system> wsys;
    cc::unique_ptr<sr::window> win;
    sg::swapchain_handle swapchain;
    sg::texture_2d capture_target;
    if (capture.active)
    {
        capture_target = ctx->persistent.create_texture_2d({.format = color_format,
                                                            .width = size[0],
                                                            .height = size[1],
                                                            .usage = sg::texture_usage::render_target
                                                                     | sg::texture_usage::copy_src});
    }
    else
    {
        auto created_window = sr::window_system::try_create({});
        if (created_window.has_error())
        {
            cc::eprintln("no window backend: {}", created_window.error().to_string());
            cc::eprintln("run with --capture to render this example headless instead");
            co_return;
        }
        wsys = cc::move(created_window.value());
        win = wsys->create_window({.title = cc::string("sg — metal cube"), .width = size[0], .height = size[1]});

        auto chain = ctx->try_create_swapchain({.window = win->native_window(), .format = color_format});
        if (chain.has_error())
        {
            cc::eprintln("no swapchain: {}", chain.error().to_string());
            co_return;
        }
        swapchain = cc::move(chain.value());
    }

    auto depth = ctx->persistent.create_texture_2d(
        {.format = depth_format, .width = size[0], .height = size[1], .usage = sg::texture_usage::depth_stencil});
    auto depth_size = size;

    auto camera = orbit_camera();
    auto dragging = false;
    auto spin = tg::angle_f::make_from_degree(0.0f);
    auto frames = u32(0);
    auto last_time = now_seconds();
    auto const capture_start = capture.clock_seconds();

    while (true)
    {
        auto const time = now_seconds();
        auto const dt = float(time - last_time);
        last_time = time;

        if (!capture.active)
        {
            wsys->poll_events();
            if (win->is_close_requested() || wsys->is_quit_requested())
                break;

            for (auto const& e : wsys->events())
            {
                if (e.is_mouse_button())
                {
                    auto const& b = e.as_mouse_button();
                    if (b.button == sr::mouse_button::left)
                        dragging = b.is_down;
                }
                else if (e.is_mouse_move() && dragging)
                    camera.orbit(e.as_mouse_move().delta);
                else if (e.is_mouse_wheel())
                    camera.zoom(e.as_mouse_wheel().delta[1]);
            }

            if (win->is_minimized())
                continue; // 0x0 there, and the swapchain would resize to it

            swapchain->set_window_size(tg::vec2i(win->width(), win->height()));
            size = tg::vec2i(win->width(), win->height());
        }

        // The depth target follows the window, and is rebuilt only when the size actually moved.
        if (size != depth_size && size[0] > 0 && size[1] > 0)
        {
            depth = ctx->persistent.create_texture_2d({.format = depth_format,
                                                       .width = size[0],
                                                       .height = size[1],
                                                       .usage = sg::texture_usage::depth_stencil});
            depth_size = size;
        }

        // The cube spins on its own while nobody is dragging it, so the example shows motion with no input at all.
        // Under capture it is pinned instead: any change restarts the accumulation, and a moving cube would spend
        // the whole timeout and then fail.
        if (!dragging && !capture.active)
            spin = spin + tg::angle_f::make_from_degree(dt * 22.0f);

        auto const rt = capture.active ? capture_target.as_render_target_view() : swapchain->acquire_backbuffer();

        auto spun = camera;
        spun.yaw = spun.yaw + spin;
        auto const aspect = float(size[0]) / float(size[1] > 0 ? size[1] : 1);

        auto cmd = ctx->create_command_list();
        {
            auto info = sg::rendering_info{};
            info.color_targets.push_back(rt.cleared(tg::vec4f(0.07f, 0.08f, 0.10f, 1.0f)));
            info.depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f);

            auto scope = cmd->raster.render_to(info);
            scope.bind_pipeline(*pipeline);
            scope.set_inline_constants(cube_constants{.view_projection = spun.view_projection(aspect)});
            scope.bind_vertex_buffer(vertices.as_vertex_buffer());
            scope.bind_index_buffer(index_buffer.as_index_buffer());
            scope.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count}});
        }

        if (capture.active)
            ctx->submit_command_list(cc::move(cmd));
        else
            ctx->submit_command_list_and_present(*swapchain, cc::move(cmd));

        ctx->advance_epoch();

        // Bounds how far ahead of the GPU the loop may run, and is the frame's one suspension point.
        co_await ctx->epochs_in_flight_completion(2);
        ++frames;

        if (capture.active && frames >= capture.accumulate_frames)
        {
            auto const writing = sr::write_capture_image_async(*ctx, capture_target, capture.output_path);
            co_await cc::async_settled(writing);
            auto const* const written = writing->try_value();
            if (written == nullptr || written->has_error())
                cc::eprintln("capture failed: {}",
                             written != nullptr ? written->error().to_string()
                                                : cc::string("the readback never landed"));
            break;
        }
        if (capture.active && capture.clock_seconds() - capture_start > capture.timeout_seconds)
        {
            cc::eprintln("capture timed out");
            break;
        }
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion(); // the last frames are still in flight
    ctx->shutdown();
}
