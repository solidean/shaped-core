#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::tan

#include <cstddef> // offsetof

// Interactive smoke test: opens a real window, creates a swapchain, and draws a spinning cube with a
// different color per face, presenting every frame until the window is closed (or Esc / a safety timeout).
//
// nx::config::manual keeps it OUT of the default `dev.py test` sweep — it opens a window and needs a human
// to watch it. Run it explicitly:  uv run dev.py test "ssc::dxc + dx12 - spinning cube in a window"
// (or `dev.py test --manual`). It prefers a hardware GPU and falls back to WARP.

namespace
{
// Matches the HLSL vs_input: POSITION (float3) + COLOR (float4). tg::pos3f / tg::vec4f are standard-layout
// (a single float[] member each), so offsetof + the tightly-packed 28-byte stride hold. position is a
// tg::pos3f (a point), which lets tg::triangle3f corners drop straight in.
struct vertex
{
    tg::pos3f position;
    tg::vec4f color;
};

constexpr char const* cube_hlsl = R"(
cbuffer Mvp : register(b0, space0) { float4x4 mvp; };

struct vs_input  { float3 position : POSITION; float4 color : COLOR; };
struct vs_output { float4 position : SV_Position; float4 color : COLOR; };

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = mul(mvp, float4(input.position, 1.0)); // tg mat is column-major; DXC packs cbuffers column-major
    output.color = input.color;
    return output;
}

float4 main_ps(vs_output input) : SV_Target { return input.color; }
)";

// 36 vertices (12 triangles): six faces of a unit cube centered at the origin, one solid color each. Each
// face is a quad split into two tg::triangle3f; every triangle corner becomes a colored vertex.
cc::vector<vertex> make_cube()
{
    cc::vector<vertex> out;
    auto emit = [&](tg::triangle3f const& tri, tg::vec4f col)
    {
        for (tg::pos3f const& p : {tri.pos0, tri.pos1, tri.pos2})
            out.push_back(vertex{.position = p, .color = col});
    };
    auto quad = [&](tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d, tg::vec4f col)
    {
        emit(tg::triangle3f(a, b, c), col);
        emit(tg::triangle3f(a, c, d), col);
    };
    float const s = 0.5f;
    // corners named by sign of (x, y, z)
    tg::pos3f const ppp(s, s, s);
    tg::pos3f const ppm(s, s, -s);
    tg::pos3f const pmp(s, -s, s);
    tg::pos3f const pmm(s, -s, -s);
    tg::pos3f const mpp(-s, s, s);
    tg::pos3f const mpm(-s, s, -s);
    tg::pos3f const mmp(-s, -s, s);
    tg::pos3f const mmm(-s, -s, -s);
    quad(mmp, pmp, ppp, mpp, tg::vec4f(0.90f, 0.20f, 0.20f, 1)); // +Z front  — red
    quad(pmm, mmm, mpm, ppm, tg::vec4f(0.20f, 0.75f, 0.30f, 1)); // -Z back   — green
    quad(pmp, pmm, ppm, ppp, tg::vec4f(0.20f, 0.45f, 0.90f, 1)); // +X right  — blue
    quad(mmm, mmp, mpp, mpm, tg::vec4f(0.95f, 0.80f, 0.20f, 1)); // -X left   — yellow
    quad(mpp, ppp, ppm, mpm, tg::vec4f(0.85f, 0.30f, 0.85f, 1)); // +Y top    — magenta
    quad(mmm, pmm, pmp, mmp, tg::vec4f(0.20f, 0.80f, 0.85f, 1)); // -Y bottom — cyan
    return out;
}

// A left-handed perspective projection in tg's column-vector convention (clip = P * view_pos), z in [0, 1].
tg::mat4f perspective_lh(tg::angle_f fovy, float aspect, float z_near, float z_far)
{
    float const f = 1.0f / tg::tan(fovy * 0.5f);
    tg::mat4f p = tg::mat4f::zero;
    p[0, 0] = f / aspect;
    p[1, 1] = f;
    p[2, 2] = z_far / (z_far - z_near);
    p[3, 2] = -z_near * z_far / (z_far - z_near);
    p[2, 3] = 1.0f;
    return p;
}

// Window state — a manual test runs one at a time, so a file-scope close flag is fine.
bool s_window_closed = false;

LRESULT CALLBACK cube_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        s_window_closed = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

// Creates and shows a real overlapped window of the given client size. hwnd is null if creation fails.
HWND create_demo_window(int w, int h)
{
    static ATOM const cls = []
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = cube_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"sg_dx12_cube_window";
        return RegisterClassExW(&wc);
    }();
    (void)cls;

    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND const hwnd = CreateWindowExW(0, L"sg_dx12_cube_window", L"sg — spinning cube (Esc to close)",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                      rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd != nullptr)
        ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// Drains the window's message queue; returns false once the window has been closed.
bool pump_messages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !s_window_closed;
}
} // namespace

template <>
struct sg::vertex_layout_of<vertex>
{
    static sg::vertex_type_layout get()
    {
        return {
            .stride = sizeof(vertex),
            .attributes = {
                {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(vertex, position)},
                {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = offsetof(vertex, color)},
            }};
    }
};

TEST("ssc::dxc + dx12 - spinning cube in a window", nx::config::manual)
{
    s_window_closed = false;

    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    // Prefer a hardware GPU (this is meant to be watched); fall back to WARP; skip if neither exists.
    auto ctx_r = sg::create_dx12_context({});
    if (ctx_r.has_error())
        ctx_r = sg::create_dx12_context({.use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_handle = ctx_r.value();
    sg::context& ctx = *ctx_handle;

    HWND const hwnd = create_demo_window(1280, 720);
    if (hwnd == nullptr)
        SKIP("no interactive window station (headless host) — cannot open a window");

    auto sc_r = ctx.try_create_swapchain({.native_window_handle = hwnd, .buffer_count = 3});
    if (sc_r.has_error())
        SKIP("could not create a swapchain for the window (likely a headless / session-0 host)");
    sg::swapchain_handle const sc = sc_r.value();
    REQUIRE(sc != nullptr);

    // Compile the vertex + pixel stages from the one source.
    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = cube_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    // The only shader binding is the MVP cbuffer (b0) — supply it as inline root constants, so the layout
    // has no descriptor groups at all.
    sg::pipeline_layout_description pld;
    pld.inline_constants = sg::binding{
        .name = "Mvp",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::uniform_buffer,
        .block_size = cc::isize(sizeof(tg::mat4f)),
    };
    auto pipeline_layout = ctx.uncached.create_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.fragment_shader = cc::move(ps);
    desc.vertex_input = sg::vertex_input_layout::create<vertex>();
    desc.topology = sg::primitive_topology::triangle_list;
    desc.rasterization.cull = sg::cull_mode::none; // draw both sides; depth sorts the faces
    desc.depth_stencil.depth_test = true;
    desc.depth_stencil.depth_write = true;
    desc.depth_stencil_format = sg::pixel_format::depth32_float;
    desc.color_targets.push_back({.format = sc->format()});
    auto pipeline = ctx.uncached.create_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    // Upload the cube's vertices once, in their own list so the buffer decays to COMMON before draws read it.
    cc::vector<vertex> const cube = make_cube();
    auto vbuf = ctx.persistent.create_raw_buffer(cc::isize(cube.size()) * cc::isize(sizeof(vertex)),
                                                 sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vbuf != nullptr);
    {
        auto up = ctx.create_command_list();
        up->upload.data_to_buffer(vbuf, cc::span<vertex const>(cube));
        ctx.submit_command_list(cc::move(up));
    }

    // Depth buffer, (re)created to match the swapchain size (which follows the window).
    sg::raw_texture_handle depth_tex;
    tg::vec2i depth_size = tg::vec2i(0, 0);
    auto ensure_depth = [&](tg::vec2i size)
    {
        if (depth_tex != nullptr && size == depth_size)
            return;
        sg::texture_description dd;
        dd.format = sg::pixel_format::depth32_float;
        dd.dimension = sg::texture_dimension::d2;
        dd.width = size[0];
        dd.height = size[1];
        dd.usage = sg::texture_usage::depth_stencil;
        depth_tex = ctx.persistent.create_raw_texture(dd);
        depth_size = size;
    };

    // Frame loop: spin the cube by wall-clock time, present each frame, until the window closes / Esc /
    // a safety timeout (so an unattended run still terminates).
    LARGE_INTEGER freq = {};
    LARGE_INTEGER start = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    constexpr float max_seconds = 30.0f;

    while (pump_messages())
    {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        float const t = float(double(now.QuadPart - start.QuadPart) / double(freq.QuadPart));
        if (t > max_seconds)
            break;

        auto rt = sc->acquire_backbuffer();                        // auto-resizes to the window first
        tg::vec2i const size = tg::vec2i(rt.width(), rt.height()); // the acquired view is the size of record
        ensure_depth(size);
        sg::texture_2d const depth_typed(depth_tex);

        // model (spin) -> view (push away from the camera at the origin) -> projection.
        tg::mat3f const rot = tg::mat3f::make_rotation_y(tg::angle_f::make_from_degree(t * 50.0f))
                            * tg::mat3f::make_rotation_x(tg::angle_f::make_from_degree(t * 30.0f));
        tg::mat4f model = tg::mat4f::identity;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                model[c, r] = rot[c, r];

        tg::mat4f view = tg::mat4f::identity;
        view[3, 2] = 3.0f; // move the cube +3 along +Z (camera at origin looking down +Z)

        float const aspect = float(size[0]) / float(size[1]);
        tg::mat4f const proj = perspective_lh(tg::angle_f::make_from_degree(60.0f), aspect, 0.1f, 100.0f);
        tg::mat4f const mvp = proj * view * model;

        auto cmd = ctx.create_command_list();
        {
            auto pass = cmd->raster.render_to({
                .color_targets = {rt.cleared(tg::vec4f(0.08f, 0.09f, 0.11f, 1))},
                .depth_stencil_target = depth_typed.as_depth_stencil_view().cleared(1.0f),
            });
            cmd->raster.bind_pipeline(*pipeline);
            cmd->raster.set_inline_constants(mvp);
            cmd->raster.bind_vertex_buffers({vbuf->as_vertex_buffer<vertex>()});
            cmd->raster.draw({.vertex_range = {.offset = 0, .size = cc::isize(cube.size())}});
        }
        ctx.submit_command_list_and_present(*sc, cc::move(cmd));
        ctx.advance_epoch(sc->buffer_count()); // throttle CPU ahead of the GPU + retire finished frames
    }

    ctx.advance_epoch_and_wait_for_idle(); // drain before the swapchain / window tear down
    if (!s_window_closed)
        DestroyWindow(hwnd);
    CHECK(true); // manual visual test — reaching here means the frame loop ran and tore down cleanly
}
