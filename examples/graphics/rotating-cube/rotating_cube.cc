// A rotating cube, drawn through sg's raster path and presented through an sr window.
//
// It is the smallest program that exercises the whole graphics stack end to end: a device, a shader package
// compiled at runtime, a raster pipeline with depth, persistent vertex and index buffers, inline constants, a
// swapchain, and a GPU timestamp around the draw.
// Whichever backend the build has is the one it runs on — dx12 on Windows, vulkan elsewhere.
//
// Under `--capture` there is no window and no swapchain: the frame goes into a texture and is written out, which is
// how the committed image is produced and how the example is verified on a machine with no display at all.

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <cube_shaders.hh>

#if ROTATING_CUBE_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#else
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

#include <chrono>

using namespace cc::primitive_defines;

namespace
{
/// One corner of the cube, matching slot 0 of shaders/cube.hlsl.
/// The attribute ORDER here is what the shader's `[[vk::location]]` numbers refer to.
struct cube_vertex
{
    tg::pos3f position;
    tg::vec3f normal;
    tg::vec3f color;
};
} // namespace

template <>
struct sg::vertex_layout_of<cube_vertex>
{
    static sg::vertex_type_layout get()
    {
        return {.stride = sizeof(cube_vertex),
                .attributes = {
                    {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(cube_vertex, position)},
                    {.semantic = "NORMAL", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(cube_vertex, normal)},
                    {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(cube_vertex, color)},
                }};
    }
};

namespace
{
constexpr int cube_vertex_count = 24; // four per face: a shared corner carries three different normals
constexpr int cube_index_count = 36;

// TODO(typed-geometry): perspective / look_at belong in tg's transform module — see its docs/modules/transform.md.
// Until tg pins a handedness and a depth range there is nothing to call, so the convention lives here: left-handed,
// z into [0, 1]. examples/vdoc/cube-editor/camera.hh carries the same pair for the same reason, which is the second
// caller that says it should move.
// tg::mat is COLUMN-major and subscripts m[col, row] — that part is worth copying rather than re-deriving.

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
    tg::vec3f const normals[]
        = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};
    tg::vec3f const colors[] = {tg::vec3f(0.90f, 0.32f, 0.30f), tg::vec3f(0.30f, 0.62f, 0.90f), tg::vec3f(0.42f, 0.80f, 0.42f),
                                tg::vec3f(0.94f, 0.74f, 0.28f), tg::vec3f(0.66f, 0.44f, 0.88f), tg::vec3f(0.94f, 0.94f, 0.92f)};

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
        // sg's default front face is counter-clockwise, and the un-reversed order leaves the cube inside out.
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(base + quad[i]);
    }
    return out;
}

[[nodiscard]] double now_seconds()
{
    auto const t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
}

/// Whatever context this build has a backend for.
[[nodiscard]] cc::result<sg::context_handle> create_context()
{
#if ROTATING_CUBE_BACKEND_DX12
    auto ctx = sg::create_dx12_context({});
    if (ctx.has_error())
        ctx = sg::create_dx12_context({.use_warp = true}); // WARP draws this correctly, only slower
    return ctx;
#else
    return sg::create_vulkan_context({});
#endif
}

/// Compiles cube.hlsl and builds the pipeline for one target format.
/// Fails only when the shaders did not compile, which is the one thing worth reporting rather than drawing nothing.
[[nodiscard]] cc::result<sg::raster_pipeline_handle> build_pipeline(sg::context& ctx, sg::pixel_format color_format)
{
    auto vs = shaders::cube.vertex.main_vs->acquire(ctx);
    auto ps = shaders::cube.fragment.main_ps->acquire(ctx);
    (void)cc::try_async_blocking_get(vs);
    (void)cc::try_async_blocking_get(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        // The compiler's diagnostics ride the async's failure channel, so "it did not compile" alone throws away the
        // one thing worth reading.
        auto const* const error = compiled_vs == nullptr ? vs->try_error() : ps->try_error();
        return cc::error(cc::any_error(cc::format("cube.hlsl did not compile: {}",
                                                  error != nullptr ? error->underlying().to_string() : cc::string("the compile never ran"))));
    }

    // The only binding is the vertex stage's 64-byte view-projection block, and it rides as inline constants —
    // so there are no binding groups at all, which is why nothing here builds one.
    auto const* const constants = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    if (constants == nullptr)
        return cc::error(cc::any_error("cube.hlsl must declare the cube_constants block"));

    // The build is asynchronous; this example has nothing else to do while it runs, so it waits on the same
    // scheduler the compiles above went to.
    auto built = ctx.cached.acquire_raster_pipeline(
        {.layout = ctx.cached.acquire_pipeline_layout({.inline_constants = *constants}),
         .vertex_shader = *compiled_vs,
         .fragment_shader = *compiled_ps,
         .vertex_input = sg::vertex_input_layout::create<cube_vertex>(),
         .rasterization = {.cull = sg::cull_mode::back},
         // Both default to OFF, and solid geometry needs both — a cube drawn without them shows whichever face
         // happened to be recorded last.
         .depth_stencil = {.depth_test = true, .depth_write = true},
         .color_targets = {{.format = color_format}},
         .depth_stencil_format = sg::pixel_format::depth32_float});
    (void)cc::try_async_blocking_get(built);

    auto const* const pipeline = built->try_value();
    if (pipeline == nullptr)
    {
        auto const* const error = built->try_error();
        return cc::error(cc::any_error(cc::format("the raster pipeline did not build: {}",
                                                  error != nullptr ? error->underlying().to_string() : cc::string("the build never ran"))));
    }
    return *pipeline;
}
} // namespace

EXAMPLE("shaped-graphics/rotating-cube")
{
    // Read first: it decides whether there is a display in the picture at all.
    auto const capture = sr::capture_request::from_environment();
    if (capture.active && !capture.name.empty())
    {
        cc::eprintln("this example offers no named capture, so it cannot take {}", capture.name);
        return;
    }

    auto const color_format = sg::pixel_format::bgra8_unorm; // what write_capture_image reads back, and what a swapchain wants
    auto const size = capture.active ? capture.size : tg::vec2i(1280, 720);

    auto ctx_result = create_context();
    if (ctx_result.has_error())
    {
        cc::eprintln("no graphics device: {}", ctx_result.error().to_string());
        return;
    }
    auto const ctx = cc::move(ctx_result.value());

    // Both targets are registered and the library picks: `acquire` walks the context's accepted formats and asks
    // each compiler whether it can produce one, so the example never names a backend's shader format itself.
    auto lib = slib::shader_library();
    auto dxil = slib::create_dxc_compiler();
    auto spirv = slib::create_dxc_spirv_compiler();
    if (dxil.has_value())
        lib.add_compiler(cc::move(dxil.value()));
    if (spirv.has_value())
        lib.add_compiler(cc::move(spirv.value()));
    if (dxil.has_error() && spirv.has_error())
    {
        cc::eprintln("no shader compiler: {}", dxil.error().to_string());
        return;
    }
    lib.add_package(shaders::package());

    auto pipeline_result = build_pipeline(*ctx, color_format);
    if (pipeline_result.has_error())
    {
        cc::eprintln("{}", pipeline_result.error().to_string());
        return;
    }
    auto const pipeline = cc::move(pipeline_result.value());

    // Persistent, uploaded once: the mesh never changes, and this is what most real geometry looks like.
    auto const mesh = build_cube_mesh();
    auto const indices = build_cube_indices();
    auto const vertices = ctx->persistent.create_buffer<cube_vertex>(cube_vertex_count, sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const index_buffer = ctx->persistent.create_buffer<u16>(cube_index_count, sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);
    {
        auto cmd = ctx->create_command_list();
        cmd->upload.data_to_buffer(vertices, cc::span<cube_vertex const>(mesh));
        cmd->upload.data_to_buffer(index_buffer, cc::span<u16 const>(indices));
        ctx->submit_command_list(cc::move(cmd));
    }

    // A window and a swapchain, or a texture to render into — the frame below does not care which.
    // The window system is only brought up when there is something to show, so a capture runs where there is no
    // window backend compiled in at all.
    cc::unique_ptr<sr::window_system> wsys;
    cc::unique_ptr<sr::window> win;
    sg::swapchain_handle swapchain;
    sg::texture_2d capture_target;
    if (capture.active)
    {
        capture_target = ctx->persistent.create_texture_2d({.format = color_format,
                                                            .width = size[0],
                                                            .height = size[1],
                                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    }
    else
    {
        auto created = sr::window_system::try_create({});
        if (created.has_error())
        {
            cc::eprintln("no window backend: {}", created.error().to_string());
            cc::eprintln("run with --capture to render this example headless instead");
            return;
        }
        wsys = cc::move(created.value());
        win = wsys->create_window({.title = cc::string("sg — rotating cube"), .width = size[0], .height = size[1]});

        auto chain = ctx->try_create_swapchain({.window = win->native_window(), .format = color_format});
        if (chain.has_error())
        {
            cc::eprintln("no swapchain: {}", chain.error().to_string());
            return;
        }
        swapchain = cc::move(chain.value());
    }

    auto camera = orbit_camera();
    auto dragging = false;
    auto spin = tg::angle_f::make_from_degree(0.0f);
    auto frames = u32(0);
    auto const start = now_seconds();
    auto last_time = start;

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

            // Unconditional rather than on a resize event: only wayland reads it, where the surface has no size of
            // its own, and every other platform's chain asks its surface and ignores this.
            swapchain->set_window_size(tg::vec2i(win->width(), win->height()));
        }

        // The cube spins on its own while nobody is dragging it, so the example shows motion with no input at all.
        // Under capture it is pinned instead: any change restarts the accumulation, and a moving cube would spend
        // the whole timeout and then fail.
        if (!dragging && !capture.active)
            spin = spin + tg::angle_f::make_from_degree(dt * 22.0f);

        // A back buffer normally, the capture texture when there is no display — everything below is written
        // against a render target and cannot tell which it got.
        auto const rt = capture.active ? capture_target.as_render_target_view() : swapchain->acquire_backbuffer();

        auto spun = camera;
        spun.yaw = spun.yaw + spin;

        auto cmd = ctx->create_command_list();
        auto const before = cmd->query.record_gpu_timestamp();
        {
            // Transient, so the depth buffer is sized to THIS frame's target and recycled with the epoch — which is
            // also what makes a window resize need no handling at all.
            auto const depth = ctx->transient.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                                 .width = rt.width(),
                                                                 .height = rt.height(),
                                                                 .usage = sg::texture_usage::depth_stencil});

            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.09f, 0.10f, 0.13f, 1.0f))},
                                               .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});
            pass.bind_pipeline(*pipeline);
            pass.bind_vertex_buffers({vertices.as_vertex_buffer()});
            pass.bind_index_buffer(index_buffer.as_index_buffer());
            pass.set_inline_constants(spun.view_projection(rt.aspect_ratio()));
            pass.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count}});
        }
        auto const after = cmd->query.record_gpu_timestamp();

        if (capture.active)
            ctx->submit_command_list(cc::move(cmd));
        else
            ctx->submit_command_list_and_present(*swapchain, cc::move(cmd));

        ctx->advance_epoch(2);
        ++frames;

        // Timestamps are read after the submit, and only when both landed — an unsupported backend hands back
        // invalid ones rather than failing, so the report simply does not appear.
        if (frames % 120 == 0)
        {
            auto const t0 = before.try_get_seconds();
            auto const t1 = after.try_get_seconds();
            if (t0.has_value() && t1.has_value())
                cc::println("gpu: {:.3f} ms/frame", (t1.value() - t0.value()) * 1000.0);
        }

        if (capture.active && frames >= capture.accumulate_frames)
        {
            auto const written = sr::write_capture_image(*ctx, capture_target, capture.output_path);
            if (written.has_error())
                cc::eprintln("capture failed: {}", written.error().to_string());
            break;
        }
        if (capture.active && time - start > capture.timeout_seconds)
        {
            cc::eprintln("capture timed out");
            break;
        }
    }

    ctx->advance_epoch_and_wait_for_idle(); // the last frames are still in flight
}
