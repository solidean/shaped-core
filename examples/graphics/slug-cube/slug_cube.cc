// Slug shapes in 3D and in 2D: labels on a spinning cube's faces, a star drawn by the cube's own shader, and a caption
// over it all.
//
// Three ways a shape reaches the screen, all from one atlas:
//   - each side face's label is a run of glyph quads sr::slug_routine draws with that face's matrix, depth-tested
//     against the cube and pulled a hair toward the camera, so it lies on the face rather than fighting it;
//   - the top face's star is no quad at all: the cube's pixel shader calls module `slug`'s `coverage` with an em
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
#include <sgl_modules/slug.hh>
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

/// A heart of four cubic Béziers, the classic SVG one in a 100-unit box, y up: the curves are split into quadratics on the CPU.
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

/// Moves every instance from `first` on by `offset` in the face's plane, which is how a run laid out from 0 is centred.
void shift(cc::vector<sr::slug_instance>& instances, isize first, tg::vec2f offset)
{
    for (auto i = first; i < instances.size(); ++i)
        instances[i].origin = instances[i].origin + offset;
}

/// `text` around a circle of `radius`, reading clockwise from the top, each glyph turned to stand on the circle.
/// The size is chosen so the text closes the circle.
/// Every glyph is its own instance with its own basis, which is what a per-shape 2x2 placement buys.
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
        // Around the glyph's middle rather than its start, so it stands square to the circle.
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
    // The default capture shows the sum, the hiragana and the star; `back` turns the camera round to the circle text,
    // the heart and the sizes, which the default view cannot see.
    auto const capture = sr::capture_request::from_environment();
    auto const capture_back = capture.active && capture.name == "back";
    if (capture.active && !capture.name.empty() && !capture_back)
    {
        cc::eprintln("this example offers only the named capture `back`, so it cannot take {}", capture.name);
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
    // The atlas's textures as module `slug`'s binding, the one group sr's routine binds as well.
    auto const tables_layout = ctx->cached.acquire_binding_group_layout<sgl_modules::slug::tables>();
    auto const decal_layout = ctx->cached.acquire_binding_group_layout<shaders::decal>();

    auto const vertices = ctx->persistent.create_buffer_from_data(build_cube_mesh(star.em_bounds), sg::buffer_usage::vertex_buffer);
    auto const indices = ctx->persistent.create_buffer_from_data(build_cube_indices(), sg::buffer_usage::index_buffer);

    // A second font for the Japanese face: the UI font has no kana, so this one comes from the OS's Japanese faces.
    // Each font has its own atlas, so its shapes are a draw of their own.
    auto japanese = cc::optional<sr::slug_font>();
    for (auto const path : {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",
                            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf"})
    {
        auto candidate = sr::slug_font::load(path);
        if (candidate.has_value())
        {
            japanese.emplace_value(cc::move(candidate).value());
            break;
        }
    }

    // Every face's content never changes, so it is laid out once, in the face's own plane: x right, y up, the face
    // spanning -0.5 to 0.5.
    // One instance array per atlas; each face draws its ranges of them with its own matrix.
    struct face_draw
    {
        sr::slug_font* font = nullptr;
        int face = 0;
        isize first = 0;
        isize count = 0;
    };
    auto latin = cc::vector<sr::slug_instance>();
    auto kana = cc::vector<sr::slug_instance>();
    auto draws = cc::vector<face_draw>();
    auto const begin_run = [&](sr::slug_font& f, cc::vector<sr::slug_instance>& to, int face)
    { draws.push_back({.font = &f, .face = face, .first = to.size()}); };
    auto const end_run = [&](cc::vector<sr::slug_instance>& to) { draws.back().count = to.size() - draws.back().first; };
    auto const ink = tg::vec4f(0.98f, 0.97f, 0.92f, 1.0f);
    auto const muted = tg::vec4f(0.78f, 0.82f, 0.90f, 1.0f);

    // +z: Gauss's sum, set from pieces — a large sigma with its limits, a fraction whose bar is a rectangle shape.
    {
        begin_run(font, latin, 1);
        auto const first = latin.size();
        auto const sigma = "\xCE\xA3"; // U+03A3, spelled as UTF-8 so no source encoding can misread it
        auto const big = 0.30f;
        auto const limit_size = 0.085f;
        auto const mid = 0.15f;
        auto const baseline = -0.07f;
        auto const sigma_width = font.line_width(sigma, big);
        font.append_line(latin, sigma, tg::pos2f(0, baseline), big, ink);
        font.append_line(latin, "n", tg::pos2f((sigma_width - font.line_width("n", limit_size)) * 0.5f, baseline + big * 0.78f), limit_size, muted);
        font.append_line(latin, "i=1", tg::pos2f((sigma_width - font.line_width("i=1", limit_size)) * 0.5f, baseline - 0.10f), limit_size, muted);
        auto x = sigma_width + 0.02f;
        font.append_line(latin, "i = ", tg::pos2f(x, baseline + 0.035f), mid, ink);
        x += font.line_width("i = ", mid);
        auto const numerator = 0.11f;
        auto const top_width = font.line_width("n(n+1)", numerator);
        font.append_line(latin, "n(n+1)", tg::pos2f(x, baseline + 0.105f), numerator, ink);
        font.append_line(latin, "2", tg::pos2f(x + (top_width - font.line_width("2", numerator)) * 0.5f, baseline - 0.035f), numerator, ink);
        auto const bar = font.atlas().add(sr::compile_slug_shape(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1))))).value();
        latin.push_back(sr::make_slug_instance(bar, tg::pos2f(x, baseline + 0.075f), tg::vec2f(top_width, 0), tg::vec2f(0, 0.012f), ink));
        shift(latin, first, tg::vec2f(-(x + top_width) * 0.5f, 0));
        font.append_line(latin, "sum of 1 .. n", tg::pos2f(-font.line_width("sum of 1 .. n", 0.06f) * 0.5f, 0.33f), 0.06f, muted);
        end_run(latin);
    }

    // +x: konnichiwa in hiragana, from the Japanese font, with its reading under it in the UI font.
    {
        auto const hiragana = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"; // こんにちは, U+3053 3093 306B 3061 306F
        if (japanese.has_value())
        {
            auto& jp = japanese.value();
            auto const em = cc::min(0.84f / jp.line_width(hiragana, 1.0f), 0.2f);
            begin_run(jp, kana, 3);
            jp.append_line(kana, hiragana, tg::pos2f(-jp.line_width(hiragana, em) * 0.5f, -0.02f), em, ink);
            end_run(kana);
        }
        begin_run(font, latin, 3);
        auto const reading = japanese.has_value() ? "konnichiwa" : "(no Japanese font found)";
        font.append_line(latin, reading, tg::pos2f(-font.line_width(reading, 0.1f) * 0.5f, -0.21f), 0.1f, muted);
        end_run(latin);
    }

    // -z: text around a circle, every glyph turned by its own basis, around a heart of cubic curves.
    {
        begin_run(font, latin, 0);
        append_circle(latin, font, "SHAPES * TEXT * OUTLINES * ANY ANGLE * ", 0.36f, ink);
        auto const love = font.atlas().add(sr::compile_slug_shape(heart())).value();
        auto const s = 0.0042f;
        latin.push_back(sr::make_slug_instance(love, tg::pos2f(-50 * s, -55 * s), tg::vec2f(s, 0), tg::vec2f(0, s), tg::vec4f(1.0f, 0.62f, 0.68f, 1)));
        end_run(latin);
    }

    // -x: one word at many sizes and colours, and a line slanted by shearing its basis.
    {
        begin_run(font, latin, 2);
        struct line
        {
            f32 size = 0;
            tg::vec4f color;
        };
        line const lines[] = {{.size = 0.05f, .color = tg::vec4f(0.40f, 0.85f, 0.80f, 1)},
                              {.size = 0.08f, .color = tg::vec4f(0.98f, 0.70f, 0.30f, 1)},
                              {.size = 0.12f, .color = tg::vec4f(0.95f, 0.45f, 0.65f, 1)},
                              {.size = 0.18f, .color = ink}};
        auto y = 0.34f;
        for (auto const& l : lines)
        {
            y -= l.size * 1.05f;
            font.append_line(latin, "Slug", tg::pos2f(-0.4f, y), l.size, l.color);
        }
        font.append_line(latin, "oblique by shear", tg::pos2f(-0.4f, -0.32f), 0.085f, muted, tg::vec2f(1, 0), tg::vec2f(0.25f, 1));
        end_run(latin);
    }

    // The face labels never change, so they go up once, into buffers every frame draws ranges of.
    // What does go up per frame is any glyph an atlas gained since the last one, which each atlas's prepare covers.
    auto latin_buffer = ctx->persistent.create_buffer_from_data(latin, sg::buffer_usage::vertex_buffer);
    auto kana_buffer = kana.empty() ? sg::buffer<sr::slug_instance>()
                                    : ctx->persistent.create_buffer_from_data(kana, sg::buffer_usage::vertex_buffer);

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
    if (capture_back)
        camera.yaw = camera.yaw + tg::angle_f::make_from_degree(180.0f);
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
        font.append_line(overlay, "each side is quads from the routine, the star on top is drawn by the cube's own shader",
                         tg::pos2f(30, 86), 18.0f, tg::vec4f(0.75f, 0.78f, 0.85f, 1), tg::vec2f(1, 0), tg::vec2f(0, -1));

        auto cmd = ctx->create_command_list();
        // Uploads first, on the list but outside the pass: each atlas's new glyphs, and the caption's instances.
        if (japanese.has_value())
            japanese.value().atlas().prepare(*cmd);
        auto const overlay_prepared = sr::slug_routine::prepare(*cmd, font.atlas(), overlay); // prepares font's atlas too
        auto const tables = ctx->transient.create_binding_group(
            *cmd, tables_layout,
            sgl_modules::slug::tables{.curves = font.atlas().curve_texture().as_texture_view(),
                                      .bands = font.atlas().band_texture().as_texture_view()});
        auto const decal = ctx->transient.create_binding_group(
            *cmd, decal_layout,
            shaders::decal{.banding = star.banding,
                           .glyph = tg::vec4i(i32(star.glyph_location & 0xffff), i32(star.glyph_location >> 16), i32(star.band_info & 0xffff),
                                              i32(star.band_info >> 16)),
                           .color = tg::vec4f(0.96f, 0.80f, 0.30f, 1.0f)});
        {
            auto const depth = ctx->transient.create_texture_2d(
                {.format = depth_format, .width = rt.width(), .height = rt.height(), .usage = sg::texture_usage::depth_stencil});
            // A plain rendering rather than the cube's generated target: the routine's pipelines name slug_quads.sgl's target set,
            // and sg checks a named one against every pipeline bound in it.
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.07f, 0.08f, 0.10f, 1.0f))},
                                               .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});

            pass.bind_pipeline(**pipeline);
            pass.bind_group(0, *tables);
            pass.bind_group(1, *decal);
            pass.bind_vertex_buffers({vertices.as_vertex_buffer()});
            pass.bind_index_buffer(indices.as_index_buffer());
            pass.set_inline_constants(shaders::constants{.view_projection = view_projection}.to_block());
            pass.draw_indexed({.index_range = {.offset = 0, .size = 36}});

            // Each face's runs with its face's frame; the bias keeps them in front of the face they lie on.
            for (auto const& d : draws)
            {
                auto const& instances = d.font == &font ? latin_buffer : kana_buffer;
                (void)sr::slug_routine::execute(pass, d.font->atlas(), instances, d.first, d.count,
                                                {.object_to_clip = view_projection * face_frame(d.face), .depth_bias = 0.0005f});
            }

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

    // The atlas's textures and the label buffers are the context's, so they go before it shuts down rather than after.
    latin_buffer = sg::buffer<sr::slug_instance>();
    kana_buffer = sg::buffer<sr::slug_instance>();
    font.atlas() = sr::slug_atlas();
    if (japanese.has_value())
        japanese.value().atlas() = sr::slug_atlas();
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    ctx->shutdown();
}
