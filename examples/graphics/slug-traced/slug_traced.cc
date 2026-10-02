// Slug shapes in a ray-traced scene: labels on a spinning cube and a ring of text floating over it, all casting shadows,
// and a star the cube's top face covers itself with.
//
// Two ways a shape reaches a traced image, from one atlas:
//   - every label is geometry: sr::build_slug_blas makes each glyph a quad of two non-opaque triangles, and module
//     `slug`'s `decide` is what the scene's any-hit calls for a label, keeping a ray exactly where it meets the glyph;
//     so the camera's rays and the shadow rays see the same letters, and the text throws letter-shaped shadows;
//   - the star is a decal: where a ray meets the cube's top face, the shader asks module `slug` for its coverage at the
//     hit's em coordinate, filtered over the em span between neighbouring rays.
//
// A pixel casts a 4 × 4 grid of rays, which is what antialiases a label's hard-edged point test.
// Needs ray queries: dx12, vulkan and metal all trace natively.
//
// Controls: left-drag orbits, the wheel zooms.
// Run it:   uv run dev.py example graphics/slug-traced

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-rendering/slug_traced.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <slug_traced_shaders.hh>

#if SLUG_TRACED_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#elif SLUG_TRACED_BACKEND_METAL
#include <shaped-graphics/backends/metal/metal_context.hh>
#else
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
/// The right-handed cross product, spelled out: tg's goes through a bivector.
[[nodiscard]] tg::vec3f cross(tg::vec3f a, tg::vec3f b)
{
    return tg::vec3f(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
}

/// The six faces in the order the shader reads them: -z, +z, -x, +x, -y, +y.
constexpr tg::vec3f face_normals[] = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0),
                                      tg::vec3f(1, 0, 0),  tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};

/// A face's own axes: right and up as a viewer facing it reads them; the top face reads from the +z side.
struct face_axes
{
    tg::vec3f right;
    tg::vec3f up;
};

[[nodiscard]] face_axes axes_of(int face)
{
    auto const n = face_normals[face];
    auto const reference = face == 5 ? tg::vec3f(0, 0, -1) : face == 4 ? tg::vec3f(0, 0, 1) : tg::vec3f(0, 1, 0);
    auto const right = cross(reference, n);
    return {.right = right, .up = cross(n, right)};
}

/// A row-major 3×4 affine transform, sg::tlas_instance's layout, from three axes and an origin.
struct affine
{
    tg::vec3f x = tg::vec3f(1, 0, 0);
    tg::vec3f y = tg::vec3f(0, 1, 0);
    tg::vec3f z = tg::vec3f(0, 0, 1);
    tg::vec3f origin = tg::vec3f(0, 0, 0);

    /// This transform after `inner`: a point goes through `inner` first.
    [[nodiscard]] affine after(affine const& inner) const
    {
        auto const apply = [&](tg::vec3f v) { return x * v[0] + y * v[1] + z * v[2]; };
        return {.x = apply(inner.x), .y = apply(inner.y), .z = apply(inner.z), .origin = apply(inner.origin) + origin};
    }

    void write_to(float (&rows)[12]) const
    {
        for (auto r = 0; r < 3; ++r)
        {
            rows[r * 4 + 0] = x[r];
            rows[r * 4 + 1] = y[r];
            rows[r * 4 + 2] = z[r];
            rows[r * 4 + 3] = origin[r];
        }
    }
};

[[nodiscard]] affine spin_y(tg::angle_f a)
{
    return {.x = tg::vec3f(tg::cos(a), 0, -tg::sin(a)), .z = tg::vec3f(tg::sin(a), 0, tg::cos(a))};
}

/// A face's label plane: x along the face's right, y up, a hair off the face so a label never shares its plane.
[[nodiscard]] affine face_frame(int face)
{
    auto const a = axes_of(face);
    return {.x = a.right, .y = a.up, .z = face_normals[face], .origin = face_normals[face] * 0.503f};
}

/// The ring's plane: lying flat over the cube, read from above.
[[nodiscard]] affine ring_frame()
{
    return {.x = tg::vec3f(1, 0, 0), .y = tg::vec3f(0, 0, -1), .z = tg::vec3f(0, 1, 0), .origin = tg::vec3f(0, 0.86f, 0)};
}

/// The unit cube as 12 triangles, two per face in face order; every corner carries the star's em box, with a margin.
struct cube_mesh
{
    cc::vector<tg::vec3f> positions;
    cc::vector<tg::vec2f> em;
};

[[nodiscard]] cube_mesh build_cube(tg::aabb2f star_em)
{
    auto const centre = tg::vec2f((star_em.min[0] + star_em.max[0]) * 0.5f, (star_em.min[1] + star_em.max[1]) * 0.5f);
    auto const half = cc::max(star_em.max[0] - star_em.min[0], star_em.max[1] - star_em.min[1]) * 0.65f;

    auto mesh = cube_mesh();
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = face_normals[face];
        auto const a = axes_of(face);
        tg::vec2f const corners[] = {tg::vec2f(-1, -1), tg::vec2f(1, -1), tg::vec2f(1, 1), tg::vec2f(-1, 1)};
        int const order[] = {0, 1, 2, 0, 2, 3};
        for (auto const i : order)
        {
            auto const c = corners[i];
            mesh.positions.push_back((n + a.right * c[0] + a.up * c[1]) * 0.5f);
            mesh.em.push_back(centre + c * half);
        }
    }
    return mesh;
}

/// A five-pointed star as one self-crossing contour, filled even-odd.
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

/// A heart of cubic Béziers in a 100-unit box, y up, split into quadratics on the CPU.
[[nodiscard]] sr::slug_outline heart()
{
    auto const p = [](f32 x, f32 y) { return tg::pos2f(x, 100.0f - y); };
    auto o = sr::slug_outline();
    o.move_to(p(50, 30));
    o.cubic_to(p(50, 27), p(45, 15), p(25, 15), 0.05f);
    o.cubic_to(p(0, 15), p(0, 42.5f), p(0, 42.5f), 0.05f);
    o.cubic_to(p(0, 60), p(20, 77), p(50, 95), 0.05f);
    o.cubic_to(p(80, 77), p(100, 60), p(100, 42.5f), 0.05f);
    o.cubic_to(p(100, 42.5f), p(100, 15), p(75, 15), 0.05f);
    o.cubic_to(p(60, 15), p(50, 27), p(50, 30), 0.05f);
    o.close();
    return o;
}

/// `text` around a circle of `radius`, reading clockwise from the top, each glyph standing on the circle.
void append_circle(cc::vector<sr::slug_instance>& out, sr::slug_font& font, cc::string_view text, f32 radius, tg::vec4f color)
{
    auto const size = 6.2831853f * radius / font.line_width(text, 1.0f);
    auto const unit = size / f32(font.face().units_per_em());
    auto angle = 1.5707963f;
    for (auto const c : text) // ASCII only, so one byte is one character
    {
        auto const g = font.face().glyph_for(char32_t(u8(c)));
        auto const id = g.has_value() ? g.value() : babel::font::glyph_id::notdef;
        auto const advance = f32(font.face().horizontal(id).advance) * unit;
        auto const a = tg::angle_f::make_from_radians(angle - advance * 0.5f / radius);
        auto const up = tg::vec2f(tg::cos(a), tg::sin(a));
        auto const right = tg::vec2f(up[1], -up[0]);
        auto const start = tg::angle_f::make_from_radians(angle);
        auto const at = tg::pos2f(radius * tg::cos(start), radius * tg::sin(start));
        if (auto const shape = font.glyph(id); shape.has_value() && shape.value().is_drawable)
            out.push_back(sr::make_slug_instance(shape.value(), at, right * unit, up * unit, color));
        angle -= advance / radius;
    }
}

struct orbit_camera
{
    float distance = 3.6f;
    tg::angle_f yaw = tg::angle_f::make_from_degree(30.0f);
    tg::angle_f pitch = tg::angle_f::make_from_degree(32.0f);

    [[nodiscard]] tg::vec3f eye() const
    {
        auto const cp = tg::cos(pitch);
        return tg::vec3f(cp * tg::sin(yaw), tg::sin(pitch), cp * tg::cos(yaw)) * distance;
    }

    void orbit(tg::vec2f drag)
    {
        auto const limit = tg::angle_f::make_from_degree(89.0f);
        yaw = yaw - tg::angle_f::make_from_degree(drag[0] * 0.35f);
        pitch = cc::clamp(pitch + tg::angle_f::make_from_degree(drag[1] * 0.35f), -limit, limit);
    }

    void zoom(float ticks) { distance = cc::clamp(distance * tg::pow(1.12f, -ticks), 2.0f, 40.0f); }
};

[[nodiscard]] cc::shared_async<cc::result<sg::context_handle>> create_context()
{
#if SLUG_TRACED_BACKEND_DX12
    co_return sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp});
#elif SLUG_TRACED_BACKEND_METAL
    co_return sg::create_metal_context({});
#else
    co_return sg::create_vulkan_context({});
#endif
}
} // namespace

ASYNC_EXAMPLE("graphics/slug-traced")
{
    auto const capture = sr::capture_request::from_environment();
    if (capture.active && !capture.name.empty())
    {
        cc::eprintln("this example offers no named captures, so it cannot take {}", capture.name);
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
    if (!ctx->supports(sg::feature::ray_query))
    {
        cc::eprintln("this device has no ray queries, which every ray of this example is");
        ctx->shutdown();
        co_return;
    }

    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());
    sr::add_shader_packages(lib);

    auto const building = shaders::slug_traced.trace_view.acquire_pipeline(*ctx);
    co_await cc::async_settled(building);
    auto const* const pipeline = building->try_value();
    if (pipeline == nullptr)
    {
        auto const* const error = building->try_error();
        cc::eprintln("the trace's pipeline did not build: {}", error != nullptr ? error->underlying().to_string() : cc::string("never ran"));
        ctx->shutdown();
        co_return;
    }

    // Everything lives in the UI font's atlas, so one `slug.tables` group serves the labels and the star.
    auto const star = font.atlas().add(sr::compile_slug_shape(pentagram(100.0f))).value();
    auto const love = font.atlas().add(sr::compile_slug_shape(heart())).value();

    // Every label is laid out once in its plane, x right and y up, and every run of them becomes a BLAS of its own.
    // The scene's instance table keeps each run's first record, which is how its any-hit hands `slug.decide` a glyph's.
    struct run
    {
        isize first = 0;
        isize count = 0;
        int face = -1; // -1 is the ring over the cube
    };
    auto labels = cc::vector<sr::slug_instance>();
    auto runs = cc::vector<run>();
    auto const begin_run = [&](int face) { runs.push_back({.first = labels.size(), .face = face}); };
    auto const end_run = [&] { runs.back().count = labels.size() - runs.back().first; };
    auto const ink = tg::vec4f(0.98f, 0.97f, 0.92f, 1.0f);
    auto const centred = [&](cc::string_view text, f32 y, f32 em, tg::vec4f color)
    { font.append_line(labels, text, tg::pos2f(-font.line_width(text, em) * 0.5f, y), em, color); };

    begin_run(1);
    centred("Slug", 0.02f, 0.30f, ink);
    centred("traced", -0.20f, 0.14f, tg::vec4f(0.80f, 0.86f, 0.98f, 1));
    end_run();

    begin_run(3);
    centred("any-hit", 0.04f, 0.17f, ink);
    centred("a point test per ray", -0.14f, 0.07f, tg::vec4f(0.98f, 0.86f, 0.60f, 1));
    end_run();

    begin_run(0);
    {
        auto const s = 0.0050f;
        labels.push_back(sr::make_slug_instance(love, tg::pos2f(-50 * s, -50 * s), tg::vec2f(s, 0), tg::vec2f(0, s), tg::vec4f(1.0f, 0.55f, 0.62f, 1)));
    }
    end_run();

    begin_run(2);
    centred("shadows", 0.04f, 0.15f, ink);
    centred("from the outlines", -0.12f, 0.07f, tg::vec4f(0.70f, 0.95f, 0.85f, 1));
    end_run();

    begin_run(-1);
    append_circle(labels, font, "SHAPES CAST SHADOWS * TEXT IN A TRACED SCENE * ", 1.05f, tg::vec4f(1.0f, 0.82f, 0.36f, 1));
    end_run();

    auto const cube = build_cube(star.em_bounds);
    auto const cube_positions = ctx->persistent.create_buffer_from_data(
        cube.positions, sg::buffer_usage::readonly_buffer | sg::buffer_usage::accel_structure_build_input);
    auto const cube_em = ctx->persistent.create_buffer_from_data(cube.em, sg::buffer_usage::readonly_buffer);

    // The scene's instance table, by instance id: the ground and the cube have no labels, and run i is instance 2 + i.
    auto first_records = cc::vector<i32>{-1, -1};
    for (auto const& r : runs)
        first_records.push_back(i32(r.first));
    auto const first_shapes = ctx->persistent.create_buffer_from_data(first_records, sg::buffer_usage::readonly_buffer);
    tg::vec3f const ground_positions[] = {tg::vec3f(-8, -0.5f, -8), tg::vec3f(8, -0.5f, -8), tg::vec3f(8, -0.5f, 8),
                                          tg::vec3f(-8, -0.5f, -8), tg::vec3f(8, -0.5f, 8), tg::vec3f(-8, -0.5f, 8)};
    auto const ground_vertices = ctx->persistent.create_buffer_from_data(ground_positions, sg::buffer_usage::accel_structure_build_input);

    // The structures that never change are built once: the ground, the cube, every run's quads, and the records.
    auto setup = ctx->create_command_list();
    auto const records = sr::upload_slug_records(*setup, font.atlas(), labels);
    auto const ground_geometry = sg::blas_triangles{.vertices = ground_vertices.raw(), .vertex_count = 6};
    auto const ground_blas = setup->raytracing.build_blas(cc::span<sg::blas_triangles const>(&ground_geometry, 1));
    auto const cube_geometry = sg::blas_triangles{.vertices = cube_positions.raw(), .vertex_count = cube.positions.size()};
    auto const cube_blas = setup->raytracing.build_blas(cc::span<sg::blas_triangles const>(&cube_geometry, 1));
    auto run_blases = cc::vector<sg::blas_handle>();
    for (auto const& r : runs)
        run_blases.push_back(sr::build_slug_blas(*setup, cc::span<sr::slug_instance const>(labels).subspan({.offset = r.first, .size = r.count})));
    ctx->submit_command_list(cc::move(setup));

    auto const image = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                          .width = size[0],
                                                          .height = size[1],
                                                          .usage = sg::texture_usage::image | sg::texture_usage::texture});

    auto wsys = cc::unique_ptr<sr::window_system>();
    auto win = cc::unique_ptr<sr::window>();
    auto swapchain = sg::swapchain_handle();
    auto capture_target = sg::texture_2d();
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
            ctx->shutdown();
            co_return;
        }
        wsys = cc::move(created_window.value());
        win = wsys->create_window({.title = cc::string("sr — Slug, traced"), .width = size[0], .height = size[1]});
        auto chain = ctx->try_create_swapchain({.window = win->native_window(), .format = color_format});
        if (chain.has_error())
        {
            cc::eprintln("no swapchain: {}", chain.error().to_string());
            ctx->shutdown();
            co_return;
        }
        swapchain = cc::move(chain.value());
    }

    sr::blit_routine::prewarm(*ctx, color_format);
    (void)co_await ctx->routines.idle_completion();

    auto const tables_layout = ctx->cached.acquire_binding_group_layout<sgl_modules::slug::tables>();
    auto const shapes_layout = ctx->cached.acquire_binding_group_layout<sgl_modules::slug::shapes>();
    auto const scene_layout = ctx->cached.acquire_binding_group_layout<shaders::scene>();

    auto camera = orbit_camera();
    auto dragging = false;
    auto spin = tg::angle_f::make_from_degree(-20.0f);
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

        // The cube and its labels turn together, and the ring the other way.
        auto const turn = spin_y(spin);
        auto instances = cc::vector<sg::tlas_instance>();
        instances.push_back({.blas = ground_blas, .instance_id = 0});
        instances.push_back({.blas = cube_blas, .instance_id = 1, .cull_mode = sg::instance_cull_mode::none});
        turn.write_to(instances.back().transform);
        for (auto i = isize(0); i < runs.size(); ++i)
        {
            auto const& r = runs[i];
            auto const frame = r.face < 0 ? spin_y(-spin * 0.5f).after(ring_frame()) : turn.after(face_frame(r.face));
            instances.push_back({.blas = run_blases[i], .instance_id = u32(2 + i), .cull_mode = sg::instance_cull_mode::none});
            frame.write_to(instances.back().transform);
        }

        // A pinhole camera: forward to the image's centre, right and up to its edges.
        auto const eye = camera.eye();
        auto const forward = tg::normalize(-eye);
        auto const right = tg::normalize(cross(forward, tg::vec3f(0, 1, 0)));
        auto const up = cross(right, forward);
        auto const half_height = tg::tan(tg::angle_f::make_from_degree(45.0f) / 2.0f);
        auto const aspect = f32(size[0]) / f32(size[1]);

        auto cmd = ctx->create_command_list();
        auto const tlas = cmd->raytracing.build_tlas(instances);
        auto const tables = ctx->transient.create_binding_group(
            *cmd, tables_layout,
            sgl_modules::slug::tables{.curves = font.atlas().curve_texture().as_texture_view(),
                                      .bands = font.atlas().band_texture().as_texture_view()});
        auto const shapes = ctx->transient.create_binding_group(*cmd, shapes_layout,
                                                                sgl_modules::slug::shapes{.instances = records.as_readonly_buffer()});
        auto const scene = ctx->transient.create_binding_group(
            *cmd, scene_layout,
            shaders::scene{.world = tlas->as_view(),
                           .cube_positions = cube_positions.as_readonly_buffer(),
                           .cube_em = cube_em.as_readonly_buffer(),
                           .first_shapes = first_shapes.as_readonly_buffer(),
                           .image = image.as_image_view<sg::pixel_format::rgba8_unorm>(),
                           .eye = eye,
                           .forward = forward,
                           .right = right * (half_height * aspect),
                           .up = up * half_height,
                           .sun = tg::vec3f(0.35f, 1.0f, 0.45f),
                           .star_banding = star.banding,
                           .star_glyph = star.glyph(),
                           .star_color = tg::vec4f(0.96f, 0.80f, 0.30f, 1.0f)});
        cmd->compute.bind_pipeline(**pipeline);
        cmd->compute.bind_group(0, *tables);
        cmd->compute.bind_group(1, *shapes);
        cmd->compute.bind_group(2, *scene);
        cmd->compute.dispatch_threads(size[0], size[1]);

        auto const rt = capture.active ? capture_target.as_render_target_view() : swapchain->acquire_backbuffer();
        {
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0, 0, 0, 1))}});
            (void)sr::blit_routine::execute(pass, image);
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

    // The atlas's textures are the context's, so they go before it shuts down rather than after.
    font.atlas() = sr::slug_atlas();
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    ctx->shutdown();
}
