// shaped-viewer on a device with neither ray tracing nor binding arrays: on the web, sv drawing into the page's canvas.
//
// A grid of OpenPBR spheres on a checkered floor, the checker a texture, lit by one soft box and a studio environment, in `render_mode::raster_bound`.
// The rows are the ones that read under direct light: dielectric, metal, coated, fuzz, anisotropic, thin film, diffuse, emission.
// Each column sweeps the row's parameter, roughness unless the row says otherwise, as shaped-viewer's openpbr-spheres does.
//
// On the web the viewer presents into `#canvas` through `viewer_config::surface`, and every frame is awaited, since WebGPU never blocks.
// Under node there is no page, so it draws headless for a few seconds and exits, which is the smoke test.
// A page opened as `web-viewer.html?model=<glb>` draws that glTF instead, fetched relative to the page through cnet's fetch client
// and imported by `sv::asset_loader`, seen from the file's first camera.
// The page can only hint at a GPU: sg asks for the high-performance adapter, and Firefox ignores that today
// (https://bugzilla.mozilla.org/show_bug.cgi?id=2016063); Windows' per-app graphics setting is what picks it.
// Natively it runs on a dx12 context withholding ray tracing and binding arrays, the device WebGPU's core describes.
// libs/graphics/shaped-viewer/docs/plans/raster-bound.md is the plan this sample tracks.
//
// Run it natively:
//   uv run dev.py example web-viewer
// On the web, build the emscripten-webgpu preset and open web/web-viewer.html, served over https or from localhost.

#include <clean-core/common/time.hh>
#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/thread/mutex.hh>
#include <imgui/imgui.h>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh> // tg::pi


#if WEB_VIEWER_BACKEND_WEBGPU
#include <clean-core/container/pinned_data.hh>
#include <clean-core/string/uri.hh>
#include <clean-net/http/fetch_http_client.hh>
#include <clean-net/io/io_system.hh>
#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-rendering/input.hh>
#else
#include <shaped-graphics/backends/dx12/dx12_context.hh> // dx12_config::withhold_binding_arrays
#endif

using namespace cc::primitive_defines;

#if WEB_VIEWER_BACKEND_WEBGPU
extern "C" EMSCRIPTEN_KEEPALIVE char* web_viewer_alloc(int size)
{
    return new char[size > 0 ? size : 1];
}

EM_JS_DEPS(web_viewer, "$stringToUTF8,$lengthBytesUTF8,$UTF8ToString");

EM_JS(char*, web_viewer_node_model_path, (), {
    if (typeof process == "undefined" || !process.env.WEB_VIEWER_MODEL)
        return 0;
    var s = String(process.env.WEB_VIEWER_MODEL);
    var n = lengthBytesUTF8(s) + 1;
    var p = _web_viewer_alloc(n);
    stringToUTF8(s, p, n);
    return p;
});

// WEB_VIEWER_LOW_POWER=1 in node's environment asks for the low-power adapter, which is the one Firefox draws on today.
EM_JS(int, web_viewer_node_low_power, (), {
    return typeof process != "undefined" && process.env.WEB_VIEWER_LOW_POWER == "1" ? 1 : 0;
});

namespace
{
/// The canvas's events since the last frame, as the viewer takes them; filled from JavaScript, drained by the frame loop.
cc::mutex<cc::vector<sr::input_event>> g_canvas_events;

/// The canvas's drawing-buffer size the page last reported, which the frame loop hands the viewer; (0, 0) for none yet.
cc::mutex<tg::vec2i> g_canvas_size = cc::mutex<tg::vec2i>(tg::vec2i(0, 0));

void push_canvas_event(sr::input_event e)
{
    g_canvas_events.lock([&](cc::vector<sr::input_event>& events) { events.push_back(cc::move(e)); });
}
} // namespace

// The canvas's listeners call these. Positions are canvas pixels, `density` canvas pixels per CSS pixel.
extern "C"
{
EMSCRIPTEN_KEEPALIVE void web_viewer_mouse_move(float x, float y, float dx, float dy, float density, int held)
{
    push_canvas_event({.pixel_density = density,
                       .payload = sr::mouse_move_event{.cursor_pos = tg::pos2f(x, y),
                                                       .delta = tg::vec2f(dx, dy),
                                                       .pixel_density = density,
                                                       .held_buttons = u8(held)}});
}

EMSCRIPTEN_KEEPALIVE void web_viewer_mouse_button(int button, int down, float x, float y, int modifiers, int clicks)
{
    push_canvas_event({.payload = sr::mouse_button_event{.button = sr::mouse_button(button),
                                                         .modifiers = sr::key_modifiers(modifiers),
                                                         .is_down = down != 0,
                                                         .clicks = clicks,
                                                         .cursor_pos = tg::pos2f(x, y)}});
}

EMSCRIPTEN_KEEPALIVE void web_viewer_mouse_wheel(float dx, float dy, float x, float y)
{
    push_canvas_event({.payload = sr::mouse_wheel_event{.delta = tg::vec2f(dx, dy), .cursor_pos = tg::pos2f(x, y)}});
}

/// The browser's next frame callback; settled by `web_viewer_animation_frame`.
cc::shared_async<cc::unit> g_animation_frame;

EMSCRIPTEN_KEEPALIVE void web_viewer_animation_frame()
{
    auto settled = cc::move(g_animation_frame);
    g_animation_frame = nullptr;
    if (settled != nullptr)
        settled->push_value({});
}

EMSCRIPTEN_KEEPALIVE void web_viewer_resize(int width, int height)
{
    g_canvas_size.lock([&](tg::vec2i& size) { size = tg::vec2i(width, height); });
}

EMSCRIPTEN_KEEPALIVE void web_viewer_key(int scancode, int character, int down, int repeat, int modifiers)
{
    push_canvas_event({.payload = sr::key_event{.scancode = sr::scancode(scancode),
                                                .character = char32_t(character),
                                                .modifiers = sr::key_modifiers(modifiers),
                                                .is_down = down != 0,
                                                .is_repeat = repeat != 0}});
}
}

EM_JS(void, web_viewer_request_animation_frame, (), { requestAnimationFrame(function() { _web_viewer_animation_frame(); }); });

/// Settles at the browser's next frame callback: a page presents once per display refresh, as a browser composites,
/// rather than as often as the GPU allows -- which Firefox answers with a 40 frames a second cadence.
[[nodiscard]] cc::shared_async<cc::unit> next_animation_frame()
{
    g_animation_frame = cc::make_async_manual<cc::unit>();
    auto waiting = g_animation_frame;
    web_viewer_request_animation_frame();
    return waiting;
}

// The page's loading screen: what is happening while there is nothing to see yet, and gone once the image is.
EM_JS(void, web_viewer_status, (char const* text), {
    var label = document.getElementById("loading-text");
    if (label)
        label.textContent = UTF8ToString(text);
});

EM_JS(void, web_viewer_loaded, (), {
    var overlay = document.getElementById("loading");
    if (overlay)
        overlay.style.display = "none";
});

// The canvas's pointer, wheel and key events, translated to sr's input as a window system would report them.
// Pointer events cover a mouse, a pen and one finger alike; a pinch is not translated yet.
EM_JS(void, web_viewer_install_canvas_input, (), {
    var canvas = document.getElementById("canvas");
    if (!canvas)
        return;
    canvas.tabIndex = 0;
    var where = function(e)
    {
        var r = canvas.getBoundingClientRect();
        var density = canvas.width / Math.max(r.width, 1);
        return {x : (e.clientX - r.left) * density, y : (e.clientY - r.top) * density, density : density};
    };
    var modifiers = function(e) { return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0); };
    // The DOM's buttons mask holds left, right, middle; sr's holds left, middle, right.
    var held = function(b) { return (b & 1) | ((b & 2) << 1) | ((b & 4) >> 1) | (b & 24); };
    canvas.addEventListener("pointerdown", function(e) {
        canvas.setPointerCapture(e.pointerId);
        canvas.focus();
        // Looking holds the right button: the cursor is locked meanwhile, so it neither leaves the canvas nor stops at an edge.
        if (e.button == 2 && canvas.requestPointerLock)
            canvas.requestPointerLock();
        var p = where(e);
        _web_viewer_mouse_button(e.button, 1, p.x, p.y, modifiers(e), Math.max(e.detail, 1));
        e.preventDefault();
    });
    canvas.addEventListener("pointerup", function(e) {
        if (e.button == 2 && document.pointerLockElement == canvas)
            document.exitPointerLock();
        var p = where(e);
        _web_viewer_mouse_button(e.button, 0, p.x, p.y, modifiers(e), Math.max(e.detail, 1));
        e.preventDefault();
    });
    canvas.addEventListener("pointermove", function(e) {
        var p = where(e);
        _web_viewer_mouse_move(p.x, p.y, e.movementX * p.density, e.movementY * p.density, p.density, held(e.buttons));
    });
    canvas.addEventListener("wheel", function(e) {
        // A tick is about 100 pixels or 3 lines; sr's positive y is away from the user, the DOM's toward.
        var per_tick = e.deltaMode == 0 ? 100 : (e.deltaMode == 1 ? 3 : 1);
        var p = where(e);
        _web_viewer_mouse_wheel(e.deltaX / per_tick, -e.deltaY / per_tick, p.x, p.y);
        e.preventDefault();
    }, {passive : false});
    canvas.addEventListener("contextmenu", function(e) { e.preventDefault(); });

    // sr::scancode's order, by KeyboardEvent.code.
    var codes = {};
    for (var i = 0; i < 26; ++i)
        codes["Key" + String.fromCharCode(65 + i)] = 1 + i;
    for (var d = 0; d < 10; ++d)
        codes["Digit" + d] = 27 + d;
    var named = [ "Enter", "Escape", "Backspace", "Tab", "Space", "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
                  "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Minus", "Equal", "BracketLeft", "BracketRight",
                  "Backslash", "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash", "ShiftLeft", "ShiftRight",
                  "ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight" ];
    for (var n = 0; n < named.length; ++n)
        codes[named[n]] = 37 + n;
    var key = function(e, down) {
        var scancode = codes[e.code] || 0;
        var character = e.key.length == 1 ? e.key.codePointAt(0) : 0;
        _web_viewer_key(scancode, character, down, e.repeat ? 1 : 0, modifiers(e));
        if (scancode != 0)
            e.preventDefault();
    };
    var toggle_fullscreen = function() {
        if (document.fullscreenElement)
            document.exitFullscreen();
        else
            (canvas.parentElement || canvas).requestFullscreen();
    };
    canvas.addEventListener("keydown", function(e) {
        if (e.code == "KeyF" && !e.repeat && !e.ctrlKey && !e.altKey && !e.metaKey)
        {
            toggle_fullscreen();
            e.preventDefault();
            return;
        }
        key(e, 1);
    });
    var button = document.getElementById("fullscreen");
    if (button)
        button.addEventListener("click", function() {
            toggle_fullscreen();
            canvas.focus();
        });

    // The drawing buffer follows the canvas's size on the page, at the screen's density, so fullscreen is sharp.
    var follow_layout = function() {
        var density = Math.min(window.devicePixelRatio || 1, 2);
        var width = Math.max(1, Math.round(canvas.clientWidth * density));
        var height = Math.max(1, Math.round(canvas.clientHeight * density));
        if (width == canvas.width && height == canvas.height)
            return;
        canvas.width = width;
        canvas.height = height;
        _web_viewer_resize(width, height);
    };
    new ResizeObserver(follow_layout).observe(canvas);
    follow_layout();
    canvas.addEventListener("keyup", function(e) { key(e, 0); });
    // A key held while the canvas loses focus never sends its release, so the camera would keep flying.
    canvas.addEventListener("blur", function() {
        for (var c in codes)
            _web_viewer_key(codes[c], 0, 0, 0, 0);
    });
});
#endif

namespace
{
constexpr int canvas_width = 1280;
constexpr int canvas_height = 720;

constexpr int column_count = 7;
constexpr float sphere_radius = 0.45f;
constexpr float spacing = 1.15f;
constexpr int rows_per_block = 4;
constexpr float block_gap_x = 2.0f;

/// An indexed UV sphere of `radius`, centered on the origin, wound so every face's geometric normal points outward.
///
/// TEMPORARY(debt-sv-examples-hand-rolled-primitive-meshes): primitive meshing belongs in typed-geometry rather than in an example.
struct sphere_mesh
{
    cc::vector<tg::pos3f> positions;
    cc::vector<u32> indices;

    /// one tangent frame per position, taking tangent space (+x tangent, +y bitangent, +z normal) to object space
    cc::vector<tg::quat_f> frames;
};

[[nodiscard]] sphere_mesh make_sphere(float radius, int rings, int segments)
{
    auto out = sphere_mesh();
    for (auto r = 0; r <= rings; ++r)
    {
        auto const theta = tg::angle_f::make_from_radians(float(r) / float(rings) * tg::pi<float>);
        for (auto s = 0; s <= segments; ++s)
        {
            auto const phi = tg::angle_f::make_from_radians(float(s) / float(segments) * 2.0f * tg::pi<float>);
            auto const n = tg::vec3f(tg::sin(theta) * tg::cos(phi), tg::cos(theta), tg::sin(theta) * tg::sin(phi));
            out.positions.push_back(tg::pos3f(radius * n[0], radius * n[1], radius * n[2]));
            auto const tangent = tg::normalize(tg::vec3f(-tg::sin(phi), 0.0f, tg::cos(phi)));
            auto const bitangent = tg::dual(tg::cross(n, tangent));
            out.frames.push_back(tg::quat_f::make_from_basis(tangent, bitangent, n).normalized());
        }
    }

    auto const index_of = [segments](int r, int s) { return u32(r * (segments + 1) + s); };
    for (auto r = 0; r < rings; ++r)
        for (auto s = 0; s < segments; ++s)
        {
            auto const a = index_of(r, s);
            auto const b = index_of(r + 1, s);
            auto const c = index_of(r + 1, s + 1);
            auto const d = index_of(r, s + 1);
            for (auto const i : {a, c, b, a, d, c})
                out.indices.push_back(i);
        }
    return out;
}

/// One row of the grid: its name, what every sphere in it binds, and which attribute its columns sweep from `from` to `to`.
struct sweep_row
{
    cc::string_view name;
    cc::vector<sv::material_attribute_binding> shared;
    cc::string_view swept = "specular_roughness";
    float from = 0.03f;
    float to = 1.0f;
};

[[nodiscard]] cc::vector<sweep_row> make_rows()
{
    using binding = sv::material_attribute_binding;
    auto rows = cc::vector<sweep_row>();
    rows.push_back({.name = "dielectric", .shared = {binding::of("base_color", tg::vec3f(0.42f, 0.44f, 0.48f))}});
    rows.push_back(
        {.name = "metal",
         .shared = {binding::of("base_color", tg::vec3f(0.94f, 0.78f, 0.38f)), binding::of("base_metalness", 1.0f),
                    binding::of("specular_color", tg::vec3f(0.97f, 0.90f, 0.72f))}});
    rows.push_back({.name = "coated",
                    .shared = {binding::of("base_color", tg::vec3f(0.55f, 0.09f, 0.07f)),
                               binding::of("coat_weight", 1.0f), binding::of("coat_roughness", 0.06f)}});
    rows.push_back(
        {.name = "fuzz",
         .shared = {binding::of("base_color", tg::vec3f(0.06f, 0.07f, 0.12f)), binding::of("specular_roughness", 0.6f),
                    binding::of("fuzz_weight", 1.0f), binding::of("fuzz_color", tg::vec3f(0.85f, 0.72f, 0.55f))},
         .swept = "fuzz_roughness"});
    rows.push_back({.name = "anisotropic",
                    .shared = {binding::of("base_color", tg::vec3f(0.90f, 0.90f, 0.93f)),
                               binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.35f)},
                    .swept = "specular_roughness_anisotropy",
                    .from = 0.0f,
                    .to = 0.95f});
    rows.push_back({.name = "thin film",
                    .shared = {binding::of("base_color", tg::vec3f(0.95f, 0.95f, 0.95f)),
                               binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.12f),
                               binding::of("thin_film_weight", 1.0f), binding::of("thin_film_ior", 1.5f)},
                    .swept = "thin_film_thickness",
                    .from = 120.0f,
                    .to = 900.0f});
    rows.push_back(
        {.name = "diffuse",
         .shared = {binding::of("base_color", tg::vec3f(0.72f, 0.55f, 0.42f)), binding::of("specular_weight", 0.0f)},
         .swept = "base_diffuse_roughness"});
    rows.push_back(
        {.name = "emission",
         .shared = {binding::of("base_color", tg::vec3f(0.05f, 0.05f, 0.06f)), binding::of("specular_roughness", 0.3f),
                    binding::of("emission_color", tg::vec3f(1.0f, 0.72f, 0.35f))},
         .swept = "emission_luminance",
         .from = 0.02f,
         .to = 1.5f});
    return rows;
}

/// The ambient occlusion every frame asks for, which the panel's sliders change.
sv::screen_ambient_occlusion g_ambient_occlusion = {};

/// The shadows every frame asks for, which the panel switches between techniques.
sv::shadow_settings g_shadows = {};

/// The reflections every frame asks for, which the panel changes.
sv::screen_space_reflections g_reflections = {};

/// The antialiasing every frame asks for, which the panel changes.
sv::temporal_antialiasing g_antialiasing = {};

/// The traced indirect light every frame asks for, which the panel changes.
sv::traced_indirect g_indirect = {};

/// A light standing in for an emissive mesh, under the id it is placed with.
struct emitter_light
{
    cc::string id;
    sv::light light;
};

/// Everything the frames place: the spheres, each with its own material, and the floor -- or the page's model instead.
struct scene_content
{
    cc::vector<sv::mesh> meshes;
    float grid_width = 0.0f;
    float grid_depth = 0.0f;

    cc::optional<sv::asset_data> model;

    /// The model's emissive meshes as rect lights, made once at load.
    cc::vector<emitter_light> emitters;
};

[[nodiscard]] scene_content make_content()
{
    auto& lib = *sv::acquire_material_library().value();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();

    auto const sphere = make_sphere(sphere_radius, 32, 48);
    auto const geometry = sv::triangle_geometry::create_from_indexed_triangles(sphere.positions, sphere.indices);
    auto const frames = sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex, sphere.frames);

    auto out = scene_content{};
    auto const rows = make_rows();
    auto const blocks = (int(rows.size()) + rows_per_block - 1) / rows_per_block;
    auto const block_pitch_x = float(column_count - 1) * spacing + block_gap_x;
    out.grid_width = float(blocks) * block_pitch_x - block_gap_x;
    out.grid_depth = float(rows_per_block - 1) * spacing;

    for (auto r = 0; r < rows.size(); ++r)
    {
        auto const& row = rows[r];
        auto const block = int(r) / rows_per_block;
        auto const depth = int(r) % rows_per_block;
        for (auto c = 0; c < column_count; ++c)
        {
            auto const t = float(c) / float(column_count - 1);
            auto overrides = row.shared;
            overrides.push_back(
                sv::material_attribute_binding::of(cc::string(row.swept), row.from + (row.to - row.from) * t));
            auto const material
                = lib.acquire(sv::material::create(cc::format("{}-{}", row.name, c), type, cc::move(overrides)));

            auto const x = (float(c) - float(column_count - 1) * 0.5f) * spacing
                         + (float(block) - float(blocks - 1) * 0.5f) * block_pitch_x;
            auto const z = float(depth) * spacing;
            out.meshes.push_back(
                sv::mesh{.name = cc::format("{}-{}", row.name, c),
                         .geometry = geometry,
                         .attributes = {frames},
                         .transform = tg::affine_transform3f::make_translation(tg::vec3f(x, sphere_radius, z)),
                         .material = material});
        }
    }

    constexpr auto h = 200.0f;
    auto ground = cc::vector<tg::pos3f>{tg::pos3f(-h, 0, -h), tg::pos3f(-h, 0, h), tg::pos3f(h, 0, h),
                                        tg::pos3f(-h, 0, -h), tg::pos3f(h, 0, h),  tg::pos3f(h, 0, -h)};
    // One uv unit a meter, so the checker below repeats every two meters of floor.
    auto ground_uvs = cc::vector<tg::vec2f>();
    for (auto const& q : ground)
        ground_uvs.push_back(tg::vec2f(q[0], q[2]) * 0.5f);
    auto ground_attributes = cc::vector<sv::mesh_attribute>();
    ground_attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, ground_uvs));
    auto floor = sv::mesh{.name = "ground",
                          .geometry = sv::triangle_geometry::create_from_positions(ground),
                          .attributes = cc::move(ground_attributes),
                          .material = lib.acquire(sv::material::create(
                              "ground", type, {sv::material_attribute_binding::of("specular_roughness", 0.55f)}))};

    // A two-tone checker, the floor's base color: a texture the bound mode samples through the material's own group.
    constexpr auto checker = 64;
    auto pixels = cc::array<byte>::create_defaulted(checker * checker * 4);
    for (auto y = 0; y < checker; ++y)
        for (auto x = 0; x < checker; ++x)
        {
            auto const light = ((x / (checker / 2)) + (y / (checker / 2))) % 2 == 0;
            auto const value = byte(light ? 96 : 40);
            auto const at = (y * checker + x) * 4;
            pixels[at + 0] = value;
            pixels[at + 1] = value;
            pixels[at + 2] = byte(light ? 100 : 44);
            pixels[at + 3] = byte(255);
        }
    floor.textures.push_back({.name = "base_color",
                              .source = {.texture = sv::texture_data::create(
                                             cc::move(pixels), sg::pixel_format::rgba8_unorm, checker, checker)}});
    out.meshes.push_back(cc::move(floor));
    return out;
}

/// The value a material binds `name` to, when it binds it to a constant of type T.
template <class T>
[[nodiscard]] cc::optional<T> constant_of(sv::material const& m, cc::string_view name)
{
    for (auto const& b : m.overrides)
        if (b.name == name && b.kind == sv::material_source_kind::constant && b.constant.size() == sizeof(T))
        {
            auto value = T{};
            auto* const bytes = reinterpret_cast<byte*>(&value);
            for (auto i = isize(0); i < isize(sizeof(T)); ++i)
                bytes[i] = b.constant[i];
            return value;
        }
    return {};
}

/// Each emissive mesh of `model` as a one-sided rect light in its plane, of its area, radiance and color.
/// The rect is the mesh's bounding rectangle in that plane, shrunk to the mesh's own area, so a disc becomes a square of
/// its area; nothing here suits a curved emitter.
///
/// TEMPORARY(feat-sv-raster-bound-emissive-instances): the bound mode lights nothing from an emissive instance, which is
/// how a glTF scene converted from a path tracer carries its light; sv's raster layer should derive these itself.
[[nodiscard]] cc::vector<emitter_light> emitters_as_rect_lights(sv::asset_data const& model)
{
    auto& lib = *sv::acquire_material_library().value();
    auto out = cc::vector<emitter_light>();
    for (auto const& mesh : model.meshes)
    {
        auto const& material = lib.get(mesh.material);
        auto const luminance = constant_of<float>(material, "emission_luminance").value_or(0.0f);
        auto const color = constant_of<tg::vec3f>(material, "emission_color").value_or(tg::vec3f(0, 0, 0));
        if (luminance <= 0.0f || (color[0] <= 0.0f && color[1] <= 0.0f && color[2] <= 0.0f))
            continue;

        auto const positions = mesh.geometry.positions;
        auto const indices = mesh.geometry.indices;
        auto const corner = [&](isize i) { return mesh.transform.transform(positions[indices.empty() ? i : isize(indices[i])]); };
        auto const corner_count = indices.empty() ? positions.size() : indices.size();
        if (corner_count < 3)
            continue;

        // The first triangle's winding says which side emits, as it says which side is the front.
        auto const a = corner(0);
        auto const normal = tg::dual(tg::cross(corner(1) - a, corner(2) - a)).normalized();
        auto const u = (corner(1) - a).normalized();
        auto const v = tg::dual(tg::cross(normal, u)).normalized();

        auto area = 0.0f;
        auto lo = tg::vec2f(1e30f, 1e30f);
        auto hi = tg::vec2f(-1e30f, -1e30f);
        for (auto i = isize(0); i + 2 < corner_count; i += 3)
        {
            area += 0.5f * tg::dual(tg::cross(corner(i + 1) - corner(i), corner(i + 2) - corner(i))).length();
            for (auto k = isize(0); k < 3; ++k)
            {
                auto const d = corner(i + k) - a;
                auto const uv = tg::vec2f(tg::dot(d, u), tg::dot(d, v));
                lo = tg::vec2f(cc::min(lo[0], uv[0]), cc::min(lo[1], uv[1]));
                hi = tg::vec2f(cc::max(hi[0], uv[0]), cc::max(hi[1], uv[1]));
            }
        }
        auto const extent = hi - lo;
        if (extent[0] <= 0.0f || extent[1] <= 0.0f || area <= 0.0f)
            continue;
        auto const shrink = tg::sqrt(area / (extent[0] * extent[1]));
        auto const middle = (lo + hi) * 0.5f;
        auto const center = a + u * middle[0] + v * middle[1];

        // A rect emits along cross(half_u, half_v), which u, v and normal make the mesh's front.
        auto light = sv::light::rect(center, u * (0.5f * extent[0] * shrink), v * (0.5f * extent[1] * shrink));
        light.nits(luminance).color(color);
        out.push_back({.id = cc::format("emitter {}", mesh.name), .light = light});
    }
    return out;
}

/// One frame's scene with the page's model: its meshes and lights, from its own first camera or framing its bounds.
/// Returns whether the view's image is final: every material compiled, so nothing draws as a stand-in.
bool author_model(sv::frame& f, sv::asset_data const& model, cc::span<emitter_light const> emitters)
{
    auto view = f.window().view();
    // A room is flown through rather than orbited: W/A/S/D and right-drag, from where the file's camera stands.
    if (!model.cameras.empty())
    {
        view.initial_fps(sv::fps_state::from_camera(model.cameras[0].camera));
        view.camera_style(sv::camera_style::fly);
    }
    else if (auto const box = model.bounds(); box.has_value())
    {
        auto const& b = box.value();
        auto const extent = b.max - b.min;
        auto const diagonal = f64(extent.length());
        view.initial_orbit({.target = tg::pos3d(0.5 * (f64(b.min[0]) + f64(b.max[0])), 0.5 * (f64(b.min[1]) + f64(b.max[1])),
                                                0.5 * (f64(b.min[2]) + f64(b.max[2]))),
                            .distance = diagonal > 0.0 ? 1.2 * diagonal : 5.0,
                            .azimuth = tg::angle_d::make_from_degree(30.0),
                            .elevation = tg::angle_d::make_from_degree(20.0)});
    }

    auto scene = view.add_scene();
    for (auto const& m : model.meshes)
        scene.add_mesh(m);
    for (auto const& l : model.lights)
        scene.add_light(l.id, l.light);
    for (auto const& e : emitters)
        scene.add_light(e.id, e.light);
    // Dimmed where the model brings its own light, so the room is lit by its emitters rather than evenly from outside.
    scene.background(sv::background::studio().scaled(emitters.empty() ? 0.6f : 0.1f));
    // A room lit by a few dim emitters, with no bounce light, is shown brighter; fixed rather than metered, so the
    // first frame -- the one a capture keeps -- is already the image.
    scene.settings({.mode = sv::render_mode::raster_bound,
                    .display = {.exposure = emitters.empty() ? 0.0f : 2.5f, .curve = sr::display_curve::neutral},
                    .ambient_occlusion = g_ambient_occlusion,
                    .shadows = g_shadows,
                    .reflections = g_reflections,
                    .antialiasing = g_antialiasing,
                    .indirect = g_indirect});
    return view.is_accumulation_converged();
}

/// One frame's scene: the grid seen from above and in front, in the bound raster mode.
/// Returns whether the view's image is final, as `author_model` does.
bool author(sv::frame& f, scene_content const& content)
{
    if (content.model.has_value())
        return author_model(f, content.model.value(), content.emitters);

    auto view = f.window().view();
    view.initial_fps({.position = tg::pos3d(0, 9.5, -10.5),
                      .yaw = tg::angle_d::make_from_degree(0.0),
                      .pitch = tg::angle_d::make_from_degree(-38.0),
                      .vertical_fov = tg::angle_d::make_from_degree(42.0)});
    view.camera_style(sv::camera_style::fly);

    auto scene = view.add_scene();
    for (auto const& m : content.meshes)
        scene.add_mesh(m);

    auto const light_u = content.grid_width * 0.5f + 2.0f;
    auto const light_v = content.grid_depth * 0.5f + 2.0f;
    scene
        .add_rect_light("key", tg::pos3f(0, 7.0f, content.grid_depth * 0.5f), tg::vec3f(light_u, 0, 0),
                        tg::vec3f(0, 0, light_v))
        .nits(150.0f / (light_u * light_v));
    scene.background(sv::background::studio().scaled(0.6f));
    scene.settings({.mode = sv::render_mode::raster_bound,
                    .ambient_occlusion = g_ambient_occlusion,
                    .shadows = g_shadows,
                    .reflections = g_reflections,
                    .antialiasing = g_antialiasing,
                    .indirect = g_indirect});
    return view.is_accumulation_converged();
}

[[nodiscard]] cc::shared_async<cc::result<sg::context_handle>> create_context()
{
#if WEB_VIEWER_BACKEND_WEBGPU
    auto const requested
        = co_await cc::async_as_result(sg::request_webgpu_context({.prefer_low_power = web_viewer_node_low_power() != 0}));
    if (requested.has_error())
        co_return cc::error(cc::any_error(cc::format("no webgpu device: {}", requested.error().underlying().to_string())));
    co_return requested.value();
#else
    co_return sg::create_dx12_context(
        {.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp,
         .withhold_raytracing = true,
         .withhold_binding_arrays = true,
         // The webgpu backend has no primitive_index, so the draws here are the pulled ones the web runs.
         .withhold_primitive_index = true});
#endif
}

#if WEB_VIEWER_BACKEND_WEBGPU
/// Whether there is a page to draw into, which under node there is not.
[[nodiscard]] bool has_page()
{
    // `==` rather than `!==`, which clang-format splits into `!= =`.
    return EM_ASM_INT({ return typeof document == 'undefined' ? 0 : 1; }) != 0;
}

/// The page's own URL; absent under node.
[[nodiscard]] cc::optional<cc::uri> page_url()
{
    auto const text = cnet::fetch_http_client::document_url();
    if (!text.has_value())
        return {};
    return cc::uri::parse(text.value());
}

/// The value of `?<name>=` in the page's URL, unescaped as written; absent without one, and under node.
[[nodiscard]] cc::optional<cc::string> page_parameter(cc::string_view name)
{
    auto const page = page_url();
    if (!page.has_value() || !page.value().has_query())
        return {};

    auto rest = page.value().query();
    while (!rest.empty())
    {
        auto const end = rest.find('&');
        auto const pair = end < 0 ? rest : rest.subview({.offset = 0, .size = end});
        if (pair.starts_with(name) && pair.size() > name.size() && pair[name.size()] == '=')
            return cc::string(pair.subview(name.size() + 1));
        rest = end < 0 ? cc::string_view() : rest.subview(end + 1);
    }
    return {};
}

/// The model the page's `?model=` names, resolved against the page.
[[nodiscard]] cc::optional<cc::uri> model_url()
{
    auto const model = page_parameter("model");
    if (!model.has_value())
        return {};
    return page_url().value().resolve(model.value());
}

/// Fetch and import the page's model, or fail with what went wrong.
[[nodiscard]] cc::shared_async<cc::result<sv::asset_data>> load_model(cc::uri url)
{
    auto io = cnet::io_system::create();
    auto client = cnet::fetch_http_client(*io);
    auto const started = cc::current_time_steady_secs();
    auto const response = cnet::http_get(client, url.text(), {.timeout = cnet::deadline::after_secs(300)});
    co_await cc::async_settled(response);
    if (auto const* const e = response->try_error(); e != nullptr)
        co_return cc::error(cc::any_error(cc::format("fetching {} failed: {}", url.text(), e->underlying().to_string())));
    auto taken = response->take_value();
    if (!taken.is_success())
        co_return cc::error(cc::any_error(cc::format("fetching {} answered {}", url.text(), taken.status())));

    auto body = cc::move(taken.body);
    cc::println("fetched {} bytes in {:.1f} s", body.size(), cc::current_time_steady_secs() - started);

    auto const name = url.path().subview(url.path().rfind('/') + 1);
    auto const loader = sv::asset_loader({});
    auto loaded = loader.load(cc::make_pinned_data(cc::move(body)), sv::asset_format::gltf, name, url.text());
    if (loaded.has_error())
        co_return cc::error(cc::any_error(cc::format("importing {} failed: {}", name, loaded.error().to_string())));
    cc::println("imported {}: {} meshes, {} lights, {} cameras, {} issues", name, loaded.value().meshes.size(),
                loaded.value().lights.size(), loaded.value().cameras.size(), loaded.value().issues.size());
    co_return cc::move(loaded.value());
}

/// `WEB_VIEWER_MODEL` from node's own environment, which emscripten's `getenv` does not see; absent in a browser.
/// It is how a WebGPU run of a model is reproduced under node, reading the file straight off the disk.
[[nodiscard]] cc::optional<cc::string> node_model_path()
{
    char* const text = web_viewer_node_model_path();
    if (text == nullptr)
        return {};
    auto out = cc::string(text);
    delete[] text;
    return out;
}

/// How many frames the headless smoke test under node draws.
/// Enough to outlast the materials' pipeline builds: a frame is under 3 ms until they land, and a shorter run ends before the first draw.
constexpr int headless_frames = 3000;
#endif

/// A frame holding the page longer than this is printed.
constexpr double slow_frame_ms = 100.0;

/// How often the page prints its frame rate.
constexpr double report_every_secs = 5.0;

/// The last frames' times, for the panel's graphs: what the CPU spent on a frame, and how long it then waited for the GPU.
struct frame_history
{
    static constexpr int size = 300;
    cc::array<float> cpu_ms = cc::array<float>::create_filled(size, 0.0f);
    cc::array<float> gpu_wait_ms = cc::array<float>::create_filled(size, 0.0f);
    cc::array<float> total_ms = cc::array<float>::create_filled(size, 0.0f);
    cc::array<float> gpu_ms = cc::array<float>::create_filled(size, 0.0f);
    int next = 0;
    int count = 0;
    bool gpu_timed = false;

    /// `interval` is from the previous frame's end to this one's, the waits for the display included.
    void add(float cpu, float gpu_wait, cc::optional<double> gpu, float interval)
    {
        cpu_ms[next] = cpu;
        gpu_wait_ms[next] = gpu_wait;
        total_ms[next] = interval;
        gpu_ms[next] = gpu.has_value() ? float(gpu.value()) : 0.0f;
        gpu_timed = gpu_timed || gpu.has_value();
        next = (next + 1) % size;
        count = cc::min(count + 1, size);
    }
};

/// The frame-time panel: a graph each of the whole frame, the CPU's part and the GPU wait, oldest left, with their
/// mean and maximum; a stutter is a spike, and which graph it is in says where it came from.
void draw_frame_panel(frame_history const& h, isize pending_work, isize streaming)
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("frame times"))
    {
        ImGui::End();
        return;
    }
    auto const stats = [&](cc::array<float> const& v, char const* label, float scale_max)
    {
        auto hi = 0.0f;
        auto sum = 0.0f;
        for (auto i = 0; i < h.count; ++i)
        {
            auto const x = v[(h.next - h.count + i + frame_history::size) % frame_history::size];
            hi = cc::max(hi, x);
            sum += x;
        }
        auto const mean = h.count > 0 ? sum / float(h.count) : 0.0f;
        auto overlay = cc::format("{}: {:.2f} mean, {:.2f} max", label, mean, hi);
        ImGui::PlotLines("##graph", v.data(), h.count == frame_history::size ? frame_history::size : h.count,
                         h.count == frame_history::size ? h.next : 0, overlay.c_str_materialize(), 0.0f, scale_max,
                         ImVec2(-1, 60));
    };
    auto const fps = [&]
    {
        auto sum = 0.0f;
        for (auto i = 0; i < h.count; ++i)
            sum += h.total_ms[i];
        return sum > 0.0f ? 1000.0f * float(h.count) / sum : 0.0f;
    }();
    ImGui::Text("%.0f frames per second over the last %d frames", fps, h.count);
    ImGui::PushID("total");
    stats(h.total_ms, "frame ms", 33.0f);
    ImGui::PopID();
    ImGui::PushID("cpu");
    stats(h.cpu_ms, "CPU ms", 16.0f);
    ImGui::PopID();
    ImGui::PushID("gpu");
    stats(h.gpu_wait_ms, "GPU wait ms", 33.0f);
    ImGui::PopID();
    // What the GPU itself spent, from timestamps: a wait far above it is time the frame spent queued or presented, not drawn.
    if (h.gpu_timed)
    {
        ImGui::PushID("gpu_time");
        stats(h.gpu_ms, "GPU time ms", 33.0f);
        ImGui::PopID();
    }
    else
        ImGui::TextUnformatted("GPU time: this device reports no timestamps");
    ImGui::Text("%d resource jobs owed, %d streaming", int(pending_work), int(streaming));
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 330), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("ambient occlusion"))
    {
        auto& ao = g_ambient_occlusion;
        ImGui::Checkbox("enabled", &ao.enabled);
        ImGui::SliderFloat("radius (m)", &ao.radius, 0.05f, 4.0f, "%.2f");
        ImGui::SliderFloat("thickness (m)", &ao.thickness, 0.01f, 2.0f, "%.2f");
        ImGui::SliderFloat("intensity", &ao.intensity, 0.25f, 4.0f, "%.2f");
        ImGui::SliderFloat("direct share", &ao.direct_share, 0.0f, 1.0f, "%.2f");
        if (ImGui::Button("defaults"))
            ao = sv::screen_ambient_occlusion{};
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("shadows"))
    {
        auto technique = int(g_shadows.technique);
        ImGui::RadioButton("none", &technique, int(sv::shadow_technique::none));
        ImGui::SameLine();
        ImGui::RadioButton("maps", &technique, int(sv::shadow_technique::maps));
        ImGui::SameLine();
        ImGui::RadioButton("traced", &technique, int(sv::shadow_technique::traced));
        g_shadows.technique = sv::shadow_technique(technique);
        if (g_shadows.technique == sv::shadow_technique::maps)
            ImGui::SliderFloat("softness", &g_shadows.softness, 0.0f, 3.0f, "%.2f");
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("reflections"))
    {
        auto& r = g_reflections;
        ImGui::Checkbox("enabled", &r.enabled);
        ImGui::Checkbox("traced where lost", &r.traced);
        ImGui::Checkbox("show history length", &r.debug_history);
        ImGui::SliderFloat("max distance (m)", &r.max_distance, 1.0f, 50.0f, "%.1f");
        ImGui::SliderFloat("thickness", &r.thickness, 0.005f, 0.3f, "%.3f");
        ImGui::SliderFloat("intensity", &r.intensity, 0.0f, 2.0f, "%.2f");
        if (ImGui::Button("defaults"))
            r = sv::screen_space_reflections{};
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 760), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("antialiasing"))
    {
        ImGui::Checkbox("temporal", &g_antialiasing.enabled);
        ImGui::SliderFloat("feedback", &g_antialiasing.feedback, 0.5f, 0.98f, "%.2f");
        ImGui::SliderFloat("clip width", &g_antialiasing.clip_width, 0.5f, 4.0f, "%.2f");
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 860), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("indirect light"))
    {
        ImGui::Checkbox("traced", &g_indirect.enabled);
        ImGui::RadioButton("half", &g_indirect.scale, 2);
        ImGui::SameLine();
        ImGui::RadioButton("quarter", &g_indirect.scale, 4);
    }
    ImGui::End();
}

/// The number `text` spells in decimal digits, its other characters ignored; 0 for none.
[[nodiscard]] int count_of(cc::string_view text)
{
    auto n = 0;
    for (auto const c : text)
        if (c >= '0' && c <= '9')
            n = n * 10 + int(c - '0');
    return n;
}
} // namespace

ASYNC_EXAMPLE("graphics/web-viewer")
{
    auto const created = create_context();
    co_await cc::async_settled(created);
    auto const* const ctx_result = created->try_value();
    if (ctx_result == nullptr || ctx_result->has_error())
    {
        FAIL(cc::format("no graphics device: {}",
                        ctx_result != nullptr ? ctx_result->error().to_string() : cc::string("the request never ran")));
    }
    auto const ctx = ctx_result->value();
    cc::println("adapter: {}", ctx->metrics.adapter().name);
#if WEB_VIEWER_BACKEND_WEBGPU
    auto content = scene_content{};
    if (auto const url = model_url(); url.has_value())
    {
        cc::println("fetching {}", url.value().text());
        web_viewer_status(cc::format("fetching {}", url.value().path()).c_str_materialize());
        auto const loaded = load_model(url.value());
        co_await cc::async_settled(loaded);
        if (loaded->try_error() != nullptr)
        {
            FAIL(loaded->try_error()->underlying().to_string());
        }
        auto model = loaded->take_value();
        if (model.has_error())
        {
            FAIL(model.error().to_string());
        }
        content.model = cc::move(model.value());
        content.emitters = emitters_as_rect_lights(content.model.value());
        if (auto const limit = page_parameter("emitters"); limit.has_value())
            content.emitters.resize_down_to(cc::min(content.emitters.size(), isize(count_of(limit.value()))));
        cc::println("{} emissive meshes stand in as rect lights", content.emitters.size());
    }
    else if (auto const path = node_model_path(); path.has_value())
    {
        auto loaded = sv::asset_loader({}).load(path.value());
        if (loaded.has_error())
        {
            FAIL(cc::format("importing {} failed: {}", path.value(), loaded.error().to_string()));
        }
        cc::println("imported {}: {} meshes", path.value(), loaded.value().meshes.size());
        content.model = cc::move(loaded.value());
        content.emitters = emitters_as_rect_lights(content.model.value());
        cc::println("{} emissive meshes stand in as rect lights", content.emitters.size());
    }
    else
        content = make_content();
#else
    // WEB_VIEWER_MODEL names a glTF file to draw instead of the spheres, for comparing a model with what the page shows.
    auto content = scene_content{};
    if (auto const path = cc::environment_variable("WEB_VIEWER_MODEL"); path.has_value())
    {
        auto loaded = sv::asset_loader({}).load(path.value());
        if (loaded.has_error())
        {
            FAIL(cc::format("importing {} failed: {}", path.value(), loaded.error().to_string()));
        }
        cc::println("imported {}: {} meshes, {} lights, {} cameras, {} issues", path.value(),
                    loaded.value().meshes.size(), loaded.value().lights.size(), loaded.value().cameras.size(),
                    loaded.value().issues.size());
        content.model = cc::move(loaded.value());
        content.emitters = emitters_as_rect_lights(content.model.value());
        if (auto const limit = cc::environment_variable("WEB_VIEWER_EMITTERS"); limit.has_value())
            content.emitters.resize_down_to(cc::min(content.emitters.size(), isize(count_of(limit.value()))));
        cc::println("{} emissive meshes stand in as rect lights", content.emitters.size());
    }
    else
        content = make_content();
#endif

#if WEB_VIEWER_BACKEND_WEBGPU
    // The page's canvas where there is a page; under node, headless until `headless_frames` and out.
    auto const page = has_page();
    auto const frame_limit = page ? 0 : headless_frames;
#else
    // Natively the viewer opens a window, or runs the capture `dev.py example --capture` asks for -- unless
    // WEB_VIEWER_FRAMES asks for a headless run of that many frames, timed like the page's, with no vsync to cap it.
    auto const page = false;
    auto const frame_limit = count_of(cc::environment_variable("WEB_VIEWER_FRAMES").value_or(cc::string()));
    if (frame_limit == 0)
    {
        for (auto f : sv::interactive(*ctx, "web-viewer", {.width = canvas_width, .height = canvas_height}))
            (void)author(f, content);
        co_await cc::async_settled(sv::background_work(*ctx));
        co_return;
    }
#endif

    auto config = sv::viewer_config{.width = canvas_width, .height = canvas_height};
#if WEB_VIEWER_BACKEND_WEBGPU
    if (page)
        config.surface = sg::swapchain_description{.window = {.platform = sg::window_platform::web_canvas,
                                                              .handle = (void*)"#canvas",
                                                              .client_size = tg::vec2i(canvas_width, canvas_height)}};
    else
        config.headless = true;
#else
    config.headless = true;
#endif

#if WEB_VIEWER_BACKEND_WEBGPU
    // `?compress=0` keeps textures uncompressed, which tells a BC7 encode apart from the rest of a texture's path.
    if (page_parameter("compress") == cc::optional<cc::string>("0"))
    {
        config.resources.textures_policy.compress = false;
        cc::println("textures stay uncompressed");
    }

    // `?ssr=0` starts without reflections and `?taa=0` without antialiasing, as `?shadows=` starts on a technique.
    if (page_parameter("ssr") == cc::optional<cc::string>("0"))
        g_reflections.enabled = false;
    if (page_parameter("taa") == cc::optional<cc::string>("0"))
        g_antialiasing.enabled = false;
    // `?debug=history` shows each traced reflection's history length in its place.
    if (page_parameter("debug") == cc::optional<cc::string>("history"))
        g_reflections.debug_history = true;

    // `?gi=0` starts without traced indirect light, `?gi=2` traces a quarter of the pixels and `?gi=4` a sixteenth.
    if (auto const gi = page_parameter("gi"); gi.has_value())
    {
        if (gi.value() == "0")
            g_indirect.enabled = false;
        else if (gi.value() == "2")
            g_indirect.scale = 2;
        else if (gi.value() == "4")
            g_indirect.scale = 4;
    }

    // `?shadows=none`, `maps` or `traced` starts on that technique, so a run without input compares them.
    if (auto const shadows = page_parameter("shadows"); shadows.has_value())
    {
        if (shadows.value() == "none")
            g_shadows.technique = sv::shadow_technique::none;
        else if (shadows.value() == "traced")
            g_shadows.technique = sv::shadow_technique::traced;
        else
            g_shadows.technique = sv::shadow_technique::maps;
    }
#endif

    auto created_viewer = sv::viewer::try_create(*ctx, "web-viewer", cc::move(config));
    if (created_viewer.has_error())
    {
        FAIL(cc::format("the viewer did not come up: {}", created_viewer.error().to_string()));
    }
    auto viewer = cc::move(created_viewer.value());
#if WEB_VIEWER_BACKEND_WEBGPU
    if (page)
    {
        web_viewer_install_canvas_input();
        web_viewer_status("compiling shaders");
    }
    auto loaded = !page;
#endif
    cc::println("drawing {} meshes in render_mode::raster_bound",
                content.model.has_value() ? content.model.value().meshes.size() : content.meshes.size());

    // A frame that holds the page past `slow_frame_ms` is printed: everything a frame does runs on the page's one thread.
    auto frames = 0;
    auto const start = cc::current_time_steady_secs();
    auto reported_at = start;
    auto frames_at_report = 0;
    auto reported_errors = cc::vector<cc::string>();

    // Where a frame's time goes, summed over a report's frames: opening it, authoring it, recording and submitting it,
    // and waiting for the GPU to let the loop go on.
    struct frame_times
    {
        f64 begin_ms = 0.0;
        f64 author_ms = 0.0;
        f64 end_ms = 0.0;
        f64 wait_ms = 0.0;
        f64 worst_ms = 0.0;
    };
    auto times = frame_times{};
    auto pending_work = isize(0);
    auto streaming = isize(0);
    auto history = frame_history{};
    auto previous_end = cc::current_time_steady_secs();
#if WEB_VIEWER_BACKEND_WEBGPU
    // `?pace=free` presents as fast as the GPU allows, for comparison.
    auto const paced = page && page_parameter("pace") != cc::optional<cc::string>("free");
#endif
    while (viewer.is_running())
    {
#if WEB_VIEWER_BACKEND_WEBGPU
        if (paced)
            co_await cc::async_settled(next_animation_frame());
#endif
        auto const begun = cc::current_time_steady_secs();
#if WEB_VIEWER_BACKEND_WEBGPU
        auto const fed = g_canvas_events.lock([](cc::vector<sr::input_event>& events) { return cc::exchange(events, {}); });
        for (auto const& e : fed)
            viewer.feed_input(e);
        if (auto const size = g_canvas_size.lock([](tg::vec2i const& s) { return s; }); size[0] > 0)
            viewer.set_surface_size(size);
#endif
        auto& f = viewer.begin_frame();
        auto const gpu_ms = f.gpu_frame_ms();
        auto const opened = cc::current_time_steady_secs();
        if (f.is_open())
        {
            auto const final_image = author(f, content);
            ++frames;
#if WEB_VIEWER_BACKEND_WEBGPU
            // Gone once every material compiled and every texture arrived; their compression may still be finishing.
            if (!loaded)
            {
                if (final_image && f.streaming_resources() == 0)
                {
                    loaded = true;
                    web_viewer_loaded();
                    cc::println("the image is final after {:.1f} s and {} frames", cc::current_time_steady_secs() - start, frames);
                }
                else
                    web_viewer_status(final_image ? "streaming textures" : "compiling shaders");
            }
#else
            (void)final_image;
#endif
            pending_work = f.pending_resource_work();
            streaming = f.streaming_resources();
            if (page && f.imgui())
                draw_frame_panel(history, pending_work, streaming);
        }
        if (!page && frames == frame_limit)
            viewer.request_close();
        auto const authored = cc::current_time_steady_secs();
        auto const ending = viewer.end_frame_async();
        auto const ended = cc::current_time_steady_secs();
        auto const held_ms = (ended - begun) * 1000.0;
        times.begin_ms += (opened - begun) * 1000.0;
        times.author_ms += (authored - opened) * 1000.0;
        times.end_ms += (ended - authored) * 1000.0;
        if (held_ms > slow_frame_ms)
            cc::println("frame {} held the page {:.0f} ms, {:.1f} s in", frames, held_ms, begun - start);
        if (begun - reported_at >= report_every_secs && frames > frames_at_report)
        {
            auto const n = f64(frames - frames_at_report);
            cc::println("{:.0f} frames per second, {} frames in {:.0f} s; per frame {:.2f} ms open, {:.2f} ms author, "
                        "{:.2f} ms record and submit, {:.2f} ms waiting for the GPU; slowest {:.1f} ms; "
                        "{} resource jobs owed, {} streaming; GPU time {}",
                        n / (begun - reported_at), frames, begun - start, times.begin_ms / n, times.author_ms / n,
                        times.end_ms / n, times.wait_ms / n, times.worst_ms, pending_work, streaming,
                        gpu_ms.has_value() ? cc::format("{:.2f} ms", gpu_ms.value())
                                                          : cc::string("not reported"));
            reported_at = begun;
            frames_at_report = frames;
            times = frame_times{};
        }
        auto const waiting = cc::current_time_steady_secs();
        co_await cc::async_settled(ending);
        auto const resumed = cc::current_time_steady_secs();
        times.wait_ms += (resumed - waiting) * 1000.0;
        times.worst_ms = cc::max(times.worst_ms, (resumed - begun) * 1000.0);
        history.add(float((waiting - begun) * 1000.0), float((resumed - waiting) * 1000.0), gpu_ms,
                    float((resumed - previous_end) * 1000.0));
        previous_end = resumed;

        // The device's validation errors are queued rather than logged, so the page prints each distinct one once.
        for (auto const& e : ctx->take_pending_errors())
        {
            auto seen = false;
            for (auto const& m : reported_errors)
                seen = seen || m == e.message;
            if (seen)
                continue;
            cc::eprintln("device error ({}): {}",
                         e.kind == sg::device_error_kind::creation_failed ? "out of memory" : "validation", e.message);
            reported_errors.push_back(e.message);
        }

        // A failed frame ends the loop with what is known about why, rather than the coroutine with the frame's error.
        if (auto const* const failed = ending->try_error(); failed != nullptr)
        {
            cc::eprintln("frame {} failed: {}", frames, failed->underlying().to_string());
            if (ctx->is_device_lost())
                cc::eprintln("the device is lost: {}", ctx->device_loss_reason());
            break;
        }
    }
    co_await viewer.finish();
    cc::println("drew {} frames in {:.1f} s", frames, cc::current_time_steady_secs() - start);
    co_await cc::async_settled(sv::background_work(*ctx));
}
