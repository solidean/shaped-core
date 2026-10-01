// Slug shapes in 3D and in 2D: labels on a spinning cube's faces, a star drawn by the cube's own shader, and a caption
// over it all.
//
// Three ways a shape reaches the screen, all from one atlas:
//   - each side face's label is a run of glyph quads sr::slug_routine draws with that face's matrix, depth-tested
//     against the cube and pulled a hair toward the camera, so it lies on the face rather than fighting it;
//   - the top face's star is no quad at all: the cube's pixel shader calls the prelude's `slug_coverage` with an em
//     coordinate the face's vertices carry, so the shape is part of the surface;
//   - the caption is the same routine with a pixel-space matrix at depth zero, so nothing covers it.
//
// The text needs a TrueType font, and shaped-core ships none yet: it takes one the operating system has.
//
// Controls: left-drag orbits, the wheel zooms.
// Run it:   uv run dev.py example graphics/slug-cube

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <slug_cube_shaders.hh>

#if SLUG_CUBE_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#elif SLUG_CUBE_BACKEND_METAL
#include <shaped-graphics/backends/metal/metal_context.hh>
#else
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
using cube_vertex = shaders::cube_vertex;

// Left-handed, z into [0, 1]: the same pair sgl-cube carries, for the same reason (tg has no transform module yet).
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

[[nodiscard]] tg::mat4f rotation_y(tg::angle_f a)
{
    auto m = tg::mat4f::identity;
    m[0, 0] = tg::cos(a);
    m[2, 0] = tg::sin(a);
    m[0, 2] = -tg::sin(a);
    m[2, 2] = tg::cos(a);
    return m;
}

struct orbit_camera
{
    float distance = 3.2f;
    tg::angle_f yaw = tg::angle_f::make_from_degree(30.0f);
    tg::angle_f pitch = tg::angle_f::make_from_degree(28.0f);

    [[nodiscard]] tg::mat4f view_projection(float aspect) const
    {
        auto const cp = tg::cos(pitch);
        auto const eye = tg::pos3f::zero + tg::vec3f(cp * tg::sin(yaw), tg::sin(pitch), cp * tg::cos(yaw)) * distance;
        return perspective(tg::angle_f::make_from_degree(45.0f), aspect, 0.1f, 100.0f) * look_at(eye, tg::pos3f::zero, tg::vec3f(0, 1, 0));
    }

    void orbit(tg::vec2f drag)
    {
        auto const limit = tg::angle_f::make_from_degree(89.0f);
        yaw = yaw + tg::angle_f::make_from_degree(drag[0] * 0.35f);
        pitch = cc::clamp(pitch + tg::angle_f::make_from_degree(drag[1] * 0.35f), -limit, limit);
    }

    void zoom(float ticks) { distance = cc::clamp(distance * tg::pow(1.12f, -ticks), 1.8f, 40.0f); }
};

/// The six faces in sgl-cube's order: -z, +z, -x, +x, -y, +y.
constexpr tg::vec3f face_normals[] = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0),
                                      tg::vec3f(1, 0, 0),  tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};
constexpr int decal_face = 5; // the top face carries the star

/// The unit cube, every face with its own normal and colour; the decal face's corners carry the star's em box.
[[nodiscard]] cc::array<cube_vertex> build_cube_mesh(tg::aabb2f star_em)
{
    tg::vec3f const colors[] = {tg::vec3f(0.80f, 0.26f, 0.24f), tg::vec3f(0.24f, 0.50f, 0.80f), tg::vec3f(0.30f, 0.66f, 0.34f),
                                tg::vec3f(0.84f, 0.62f, 0.20f), tg::vec3f(0.52f, 0.34f, 0.74f), tg::vec3f(0.22f, 0.24f, 0.30f)};

    // the star's box with a margin, so it sits inside the face rather than touching its edges
    auto const centre = tg::pos2f((star_em.min[0] + star_em.max[0]) * 0.5f, (star_em.min[1] + star_em.max[1]) * 0.5f);
    auto const half = cc::max(star_em.max[0] - star_em.min[0], star_em.max[1] - star_em.min[1]) * 0.65f;

    auto out = cc::array<cube_vertex>::create_defaulted(24);
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = face_normals[face];
        auto const u = tg::vec3f(n[1], n[2], n[0]);
        auto const v = cross3(n, u);
        for (auto corner = 0; corner < 4; ++corner)
        {
            auto const su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            auto const sv = (corner >= 2) ? 1.0f : -1.0f;
            out[face * 4 + corner] = {.position = tg::pos3f::zero + (n + u * su + v * sv) * 0.5f,
                                      .normal = n,
                                      .color = colors[face],
                                      .em = tg::vec2f(centre[0] + su * half, centre[1] - sv * half),
                                      .decal = face == decal_face ? 1.0f : 0.0f};
        }
    }
    return out;
}

[[nodiscard]] cc::array<u16> build_cube_indices()
{
    auto out = cc::array<u16>::create_defaulted(36);
    for (auto face = 0; face < 6; ++face)
    {
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(face * 4 + quad[i]);
    }
    return out;
}

/// A five-pointed star as ONE self-crossing contour, filled even-odd: its middle pentagon is the hole that shows the rule.
[[nodiscard]] sr::slug_outline pentagram(f32 radius)
{
    auto o = sr::slug_outline();
    o.fill_rule = sr::slug_fill_rule::even_odd;
    for (auto i = 0; i < 5; ++i)
    {
        auto const a = tg::angle_f::make_from_degree(90.0f + 144.0f * f32(i));
        auto const p = tg::pos2f(radius * tg::cos(a), radius * tg::sin(a));
        if (i == 0)
            o.move_to(p);
        else
            o.line_to(p);
    }
    o.close();
    return o;
}

/// The matrix taking a face's label plane to the cube's object space: x along the face's right, y up, at the face.
[[nodiscard]] tg::mat4f face_frame(int face)
{
    auto const n = face_normals[face];
    auto const up = tg::vec3f(0, 1, 0);
    auto const right = cross3(n, up);
    auto m = tg::mat4f::identity;
    for (auto i = 0; i < 3; ++i)
    {
        m[0, i] = right[i];
        m[1, i] = up[i];
        m[2, i] = n[i];
        m[3, i] = n[i] * 0.5f;
    }
    return m;
}

/// Pixel space, y down, at depth zero: the overlay's matrix.
[[nodiscard]] tg::mat4f pixels_to_clip(tg::vec2i size)
{
    auto m = tg::mat4f::identity;
    m[0, 0] = 2.0f / f32(size[0]);
    m[3, 0] = -1.0f;
    m[1, 1] = -2.0f / f32(size[1]);
    m[3, 1] = 1.0f;
    m[2, 2] = 0.0f;
    return m;
}

[[nodiscard]] cc::shared_async<cc::result<sg::context_handle>> create_context()
{
#if SLUG_CUBE_BACKEND_DX12
    co_return sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp});
#elif SLUG_CUBE_BACKEND_METAL
    co_return sg::create_metal_context({});
#else
    co_return sg::create_vulkan_context({});
#endif
}
} // namespace

ASYNC_EXAMPLE("graphics/slug-cube")
{
    auto const capture = sr::capture_request::from_environment();
    if (capture.active && !capture.name.empty())
    {
        cc::eprintln("this example offers no named capture, so it cannot take {}", capture.name);
        co_return;
    }

    auto loaded = sr::slug_font::load_system_ui_font();
    if (loaded.has_error())
    {
        cc::eprintln("no font to draw with: {}", loaded.error().to_string());
        co_return;
    }
    auto font = cc::move(loaded).value();

    auto const color_format = sg::pixel_format::bgra8_unorm;
    auto const depth_format = sg::pixel_format::depth32_float;
    auto const size = capture.active ? capture.size : tg::vec2i(1280, 720);

    auto const created = create_context();
    co_await cc::async_settled(created);
    auto const* const ctx_result = created->try_value();
    if (ctx_result == nullptr || ctx_result->has_error())
    {
        cc::eprintln("no graphics device: {}", ctx_result != nullptr ? ctx_result->error().to_string() : cc::string("the request never ran"));
        co_return;
    }
    auto const ctx = ctx_result->value();

    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());
    sr::add_shader_packages(lib);

    // The star lives in the font's atlas, so the cube's shader and the routine bind the same two textures.
    auto const star = font.atlas().add(sr::compile_slug_shape(pentagram(100.0f))).value();

    auto const building = ctx->cached.acquire_raster_pipeline(shaders::slug_cube.pipeline, {.color = color_format});
    co_await cc::async_settled(building);
    auto const* const pipeline = building->try_value();
    if (pipeline == nullptr)
    {
        auto const* const error = building->try_error();
        cc::eprintln("the cube's pipeline did not build: {}", error != nullptr ? error->underlying().to_string() : cc::string("never ran"));
        co_return;
    }
    auto const decal_layout = ctx->cached.acquire_binding_group_layout<shaders::decal>();

    auto const vertices = ctx->persistent.create_buffer_from_data(build_cube_mesh(star.em_bounds), sg::buffer_usage::vertex_buffer);
    auto const indices = ctx->persistent.create_buffer_from_data(build_cube_indices(), sg::buffer_usage::index_buffer);

    // The labels never change, so they are laid out once: one run of glyph quads per side face.
    // Each face's run is a range of one instance array, drawn with that face's own matrix.
    char const* const labels[] = {"SLUG", "SGL", "4 sides", "any size"};
    int const label_faces[] = {0, 3, 1, 2};
    auto instances = cc::vector<sr::slug_instance>();
    auto ranges = cc::vector<tg::vec2i>(); // first, count per label
    for (auto i = 0; i < 4; ++i)
    {
        auto const em = 0.2f;
        auto const width = font.line_width(labels[i], em);
        auto const first = i32(instances.size());
        font.append_line(instances, labels[i], tg::pos2f(-width * 0.5f, -em * 0.35f), em, tg::vec4f(0.98f, 0.97f, 0.92f, 1.0f));
        ranges.push_back(tg::vec2i(first, i32(instances.size()) - first));
    }

    cc::unique_ptr<sr::window_system> wsys;
    cc::unique_ptr<sr::window> win;
    sg::swapchain_handle swapchain;
    sg::texture_2d capture_target;
    if (capture.active)
    {
        capture_target = ctx->persistent.create_texture_2d(
            {.format = color_format, .width = size[0], .height = size[1], .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
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
        win = wsys->create_window({.title = cc::string("sr — Slug on a cube"), .width = size[0], .height = size[1]});
        auto chain = ctx->try_create_swapchain({.window = win->native_window(), .format = color_format});
        if (chain.has_error())
        {
            cc::eprintln("no swapchain: {}", chain.error().to_string());
            co_return;
        }
        swapchain = cc::move(chain.value());
    }

    // The routine draws the instances prepare() uploaded and the atlas textures it filled, so each draw's barrier is found
    // inside the scope, and vulkan splits the scope for it.
    // Nothing states a scope's accesses before it opens yet; libs/graphics/shaped-graphics/docs/TODO.md, "Barriers + access tracking".
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    // Both routine parametrizations this frame uses, named up front so the first frame does not decline.
    sr::slug_routine::prewarm(*ctx, {.color = color_format, .depth = depth_format});
    (void)co_await ctx->routines.idle_completion();

    auto camera = orbit_camera();
    auto dragging = false;
    auto spin = tg::angle_f::make_from_degree(0.0f);
    auto frames = u32(0);
    auto last_time = cc::current_time_steady_secs();
    auto const capture_start = capture.clock_seconds();

    while (true)
    {
        auto const time = cc::current_time_steady_secs();
        auto const dt = float(time - last_time);
        last_time = time;

        if (!capture.active)
        {
            wsys->poll_events();
            if (win->is_close_requested() || wsys->is_quit_requested())
                break;
            for (auto const& e : wsys->events())
            {
                if (e.is_mouse_button() && e.as_mouse_button().button == sr::mouse_button::left)
                    dragging = e.as_mouse_button().is_down;
                else if (e.is_mouse_move() && dragging)
                    camera.orbit(e.as_mouse_move().delta);
                else if (e.is_mouse_wheel())
                    camera.zoom(e.as_mouse_wheel().delta[1]);
            }
            if (win->is_minimized())
                continue;
            swapchain->set_window_size(tg::vec2i(win->width(), win->height()));
        }

        if (!dragging && !capture.active)
            spin = spin + tg::angle_f::make_from_degree(dt * 18.0f);

        (void)ctx->routines.tick();

        auto const rt = capture.active ? capture_target.as_render_target_view() : swapchain->acquire_backbuffer();
        auto const target_size = tg::vec2i(rt.width(), rt.height());
        auto const view_projection = camera.view_projection(rt.aspect_ratio()) * rotation_y(spin);

        // the caption, rebuilt every frame since it would carry a frame time in a real tool
        auto overlay = cc::vector<sr::slug_instance>();
        font.append_line(overlay, "Slug: text and shapes from their outlines", tg::pos2f(28, 52), 34.0f, tg::vec4f(1, 1, 1, 1),
                         tg::vec2f(1, 0), tg::vec2f(0, -1));
        font.append_line(overlay, "labels are glyph quads on each face, the star is drawn by the cube's own shader",
                         tg::pos2f(30, 86), 18.0f, tg::vec4f(0.75f, 0.78f, 0.85f, 1), tg::vec2f(1, 0), tg::vec2f(0, -1));

        auto cmd = ctx->create_command_list();
        // Uploads first, on the list but outside the pass: the atlas's new glyphs and both instance arrays.
        auto const labels_prepared = sr::slug_routine::prepare(*cmd, font.atlas(), instances);
        auto const overlay_prepared = sr::slug_routine::prepare(*cmd, font.atlas(), overlay);
        auto const decal = ctx->transient.create_binding_group(
            *cmd, decal_layout,
            shaders::decal{.curves = font.atlas().curve_texture().as_texture_view(),
                           .bands = font.atlas().band_texture().as_texture_view(),
                           .banding = star.banding,
                           .glyph = tg::vec4i(i32(star.glyph_location & 0xffff), i32(star.glyph_location >> 16), i32(star.band_info & 0xffff),
                                              i32(star.band_info >> 16)),
                           .color = tg::vec4f(0.96f, 0.80f, 0.30f, 1.0f)});
        {
            auto const depth = ctx->transient.create_texture_2d(
                {.format = depth_format, .width = rt.width(), .height = rt.height(), .usage = sg::texture_usage::depth_stencil});
            // A plain rendering rather than the cube's generated target: the routine's pipelines name slug.sgl's target set,
            // and sg checks a named one against every pipeline bound in it.
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.07f, 0.08f, 0.10f, 1.0f))},
                                               .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});

            pass.bind_pipeline(**pipeline);
            pass.bind_group(0, *decal);
            pass.bind_vertex_buffers({vertices.as_vertex_buffer()});
            pass.bind_index_buffer(indices.as_index_buffer());
            pass.set_inline_constants(shaders::constants{.view_projection = view_projection}.to_block());
            pass.draw_indexed({.index_range = {.offset = 0, .size = 36}});

            // Each label with its face's frame; the bias keeps it in front of the face it lies on.
            for (auto i = 0; i < 4; ++i)
                (void)sr::slug_routine::execute(pass, font.atlas(), labels_prepared.instances, ranges[i][0], ranges[i][1],
                                                {.object_to_clip = view_projection * face_frame(label_faces[i]), .depth_bias = 0.0005f});

            // Depth zero passes any depth the cube left.
            (void)sr::slug_routine::execute(pass, font.atlas(), overlay_prepared, {.object_to_clip = pixels_to_clip(target_size)});
        }

        if (capture.active)
            ctx->submit_command_list(cc::move(cmd));
        else
            ctx->submit_command_list_and_present(*swapchain, cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
        ++frames;

        if (capture.active && frames >= capture.accumulate_frames)
        {
            auto const writing = sr::write_capture_image_async(*ctx, capture_target, capture.output_path);
            co_await cc::async_settled(writing);
            auto const* const written = writing->try_value();
            if (written == nullptr || written->has_error())
                cc::eprintln("capture failed: {}", written != nullptr ? written->error().to_string() : cc::string("the readback never landed"));
            break;
        }
        if (capture.active && capture.clock_seconds() - capture_start > capture.timeout_seconds)
        {
            cc::eprintln("capture timed out");
            break;
        }
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion();
    ctx->shutdown();
}
