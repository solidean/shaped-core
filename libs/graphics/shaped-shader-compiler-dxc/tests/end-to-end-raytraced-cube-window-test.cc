#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

#include <chrono>

// Interactive smoke test: a real window with a spinning cube, ray-traced through the DXR pipeline.
// BLAS/TLAS + raygen/miss/closest-hit shaders + shader table + dispatch_rays — not rasterized.
// Per frame: rebuild the TLAS with the spin transform, trace one ray per pixel into an offscreen UAV image,
// then blit that image onto the swapchain back buffer.
// The blit exists because DXR writes a UAV while flip-model back buffers are render-target-only.
// Different color + simple shading per face.
//
// nx::config::manual keeps it out of the default sweep. Run it explicitly:
//   uv run dev.py test "raytraced spinning cube in a window"
// Prefers a hardware GPU, falls back to WARP; SKIPs if the device has no ray tracing.

using namespace tg::literals;

namespace
{
// --- DXR shaders (one single-entry library each, compiled at sm_6_3) --------------------------------

// Raygen: a pinhole camera (basis from the Camera cbuffer) shoots one primary ray per pixel.
// The payload color lands in the output image.
constexpr char const* raygen_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> Output : register(u0);
cbuffer Camera : register(b0)
{
    float3 cam_pos;  float _p0;
    float3 forward;  float _p1;
    float3 right_s;  float _p2; // right  * aspect * tan(fovY/2)
    float3 up_s;     float _p3; // up     * tan(fovY/2)
};

struct Payload { float4 color; };

[shader("raygeneration")]
void RayGen()
{
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 ndc = (float2(px) + 0.5) / float2(dim) * 2.0 - 1.0; // [-1, 1]
    float3 dir = normalize(forward + right_s * ndc.x - up_s * ndc.y);

    RayDesc ray;
    ray.Origin = cam_pos;
    ray.Direction = dir;
    ray.TMin = 0.001;
    ray.TMax = 1000.0;

    Payload payload;
    payload.color = float4(0, 0, 0, 1);
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    Output[px] = payload.color;
}
)";

constexpr char const* miss_hlsl = R"(
struct Payload { float4 color; };
[shader("miss")]
void Miss(inout Payload payload) { payload.color = float4(0.08, 0.09, 0.11, 1.0); } // background
)";

// Closest-hit: face index = PrimitiveIndex()/2 (two triangles per face).
// Flat face color with a cheap lambert against a fixed light, using the (rotating) object->world normal.
constexpr char const* closest_hit_hlsl = R"(
struct Payload { float4 color; };
struct Attributes { float2 bary; };

static const float3 palette[6] = {
    float3(0.90, 0.20, 0.20), // +Z red
    float3(0.20, 0.75, 0.30), // -Z green
    float3(0.20, 0.45, 0.90), // +X blue
    float3(0.95, 0.80, 0.20), // -X yellow
    float3(0.85, 0.30, 0.85), // +Y magenta
    float3(0.20, 0.80, 0.85), // -Y cyan
};
static const float3 normals[6] = {
    float3(0, 0, 1), float3(0, 0, -1), float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0),
};

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attribs)
{
    uint face = PrimitiveIndex() / 2;
    float3 n = normalize(mul((float3x3)ObjectToWorld3x4(), normals[face]));
    float3 light = normalize(float3(0.4, 0.8, -0.5));
    float shade = 0.25 + 0.75 * saturate(dot(n, light));
    payload.color = float4(palette[face] * shade, 1.0);
}
)";

// --- fullscreen blit (raster): sample the ray-traced image onto the back buffer --------------------

constexpr char const* blit_hlsl = R"(
Texture2D<float4> Src : register(t0);
SamplerState Samp : register(s0);

struct v2p { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

v2p main_vs(uint vid : SV_VertexID)
{
    v2p o;
    o.uv = float2((vid << 1) & 2, vid & 2); // {(0,0),(2,0),(0,2)} — a screen-covering triangle
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 main_ps(v2p i) : SV_Target { return Src.SampleLevel(Samp, i.uv, 0); }
)";

// Cube positions: six faces, each a quad split into two triangles, in the palette's face order.
// That order is what makes PrimitiveIndex()/2 in the closest-hit pick the matching color/normal.
// 36 positions, non-indexed.
cc::vector<tg::pos3f> make_cube_positions()
{
    cc::vector<tg::pos3f> out;
    auto quad = [&](tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d)
    {
        for (tg::pos3f const& p : {a, b, c, a, c, d})
            out.push_back(p);
    };
    auto const s = 0.5f;
    auto const ppp = tg::pos3f(s, s, s);
    auto const ppm = tg::pos3f(s, s, -s);
    auto const pmp = tg::pos3f(s, -s, s);
    auto const pmm = tg::pos3f(s, -s, -s);
    auto const mpp = tg::pos3f(-s, s, s);
    auto const mpm = tg::pos3f(-s, s, -s);
    auto const mmp = tg::pos3f(-s, -s, s);
    auto const mmm = tg::pos3f(-s, -s, -s);
    quad(mmp, pmp, ppp, mpp); // +Z
    quad(pmm, mmm, mpm, ppm); // -Z
    quad(pmp, pmm, ppm, ppp); // +X
    quad(mmm, mmp, mpp, mpm); // -X
    quad(mpp, ppp, ppm, mpm); // +Y
    quad(mmm, pmm, pmp, mmp); // -Y
    return out;
}

// Camera constants, matching the raygen cbuffer: float3 + pad → 16-byte slots, 64 bytes total.
struct camera_data
{
    tg::vec3f cam_pos;
    float _p0 = 0;
    tg::vec3f forward;
    float _p1 = 0;
    tg::vec3f right_s;
    float _p2 = 0;
    tg::vec3f up_s;
    float _p3 = 0;
};

// Pinhole-camera basis: a fixed camera at `eye` looking at the origin, horizontal span pre-scaled by aspect.
// Left-handed: +Z points into the scene.
camera_data make_camera(tg::vec3f eye, float aspect, tg::angle_f fovy)
{
    auto const up = tg::vec3f(0, 1, 0);
    auto const forward = (-eye).normalized();
    auto const right = tg::dual(tg::cross(up, forward)).normalized();
    auto const true_up = tg::dual(tg::cross(forward, right));
    auto const t = (fovy * 0.5f).tan();

    return {.cam_pos = eye, .forward = forward, .right_s = right * (aspect * t), .up_s = true_up * t};
}

// One TLAS instance placing the cube under the pure rotation `rot`.
// tlas_instance::transform is a row-major 3x4 with translation in column 3.
// tg::mat3 is column-major, so math element [row][col] reads as rot[col, row].
// Culling is off so the cube stays solid regardless of winding.
sg::tlas_instance make_cube_instance(sg::blas_handle blas, tg::mat3f const& rot)
{
    auto inst = sg::tlas_instance{.blas = cc::move(blas), .cull_mode = sg::instance_cull_mode::none};
    for (int r = 0; r < 3; ++r)
    {
        inst.transform[r * 4 + 0] = rot[0, r];
        inst.transform[r * 4 + 1] = rot[1, r];
        inst.transform[r * 4 + 2] = rot[2, r];
        inst.transform[r * 4 + 3] = 0.0f; // no translation — cube stays centered
    }
    return inst;
}

// --- window (a real overlapped window, shown; distinct class name from the raster cube demo) --------

bool s_window_closed = false;

LRESULT CALLBACK rt_cube_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
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

HWND create_demo_window(int w, int h)
{
    static ATOM const cls = []
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = rt_cube_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"sg_dx12_rt_cube_window";
        return RegisterClassExW(&wc);
    }();
    (void)cls;

    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND const hwnd = CreateWindowExW(0, L"sg_dx12_rt_cube_window", L"sg — ray-traced spinning cube (Esc to close)",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                      rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd != nullptr)
        ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

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

TEST("ssc::dxc + dx12 - raytraced spinning cube in a window", nx::config::manual)
{
    s_window_closed = false;

    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({});
    if (ctx_r.has_error())
        ctx_r = sg::create_dx12_context({.use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_handle = ctx_r.value();
    sg::context& ctx = *ctx_handle;

    // Ray tracing is a runtime capability — gate on it.
    {
        auto probe = ctx.create_command_list();
        bool const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    HWND const hwnd = create_demo_window(1280, 720);
    if (hwnd == nullptr)
        SKIP("no interactive window station (headless host) — cannot open a window");

    auto sc_r = ctx.try_create_swapchain({.native_window_handle = hwnd, .buffer_count = 3});
    if (sc_r.has_error())
        SKIP("could not create a swapchain for the window (likely a headless / session-0 host)");
    sg::swapchain_handle const sc = sc_r.value();
    REQUIRE(sc != nullptr);

    auto compile = [&](char const* source, char const* entry, sg::shader_stage stage,
                       ssc::dxc::shader_model model) -> sg::compiled_shader
    {
        auto r = comp.value().compile({.source = source, .entry_point = entry, .stage = stage, .model = model});
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };

    // --- ray-tracing pipeline + shader table --------------------------------------------------------
    // DXR needs lib_6_3+.
    auto raygen = compile(raygen_hlsl, "RayGen", sg::shader_stage::raygen, ssc::dxc::shader_model::sm_6_3);
    auto miss = compile(miss_hlsl, "Miss", sg::shader_stage::miss, ssc::dxc::shader_model::sm_6_3);
    auto closest_hit
        = compile(closest_hit_hlsl, "ClosestHit", sg::shader_stage::closest_hit, ssc::dxc::shader_model::sm_6_3);

    // The global root signature comes from the raygen's reflected bindings (scene t0, Output u0, Camera b0).
    auto rt_group_layout = ctx.uncached.create_binding_group_layout(raygen.bindings);
    REQUIRE(rt_group_layout != nullptr);
    auto rt_pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {rt_group_layout}});
    REQUIRE(rt_pipeline_layout != nullptr);

    auto rpd = sg::raytracing_pipeline_description{.layout = rt_pipeline_layout, .max_payload_size = sizeof(float) * 4};
    auto const raygen_h = rpd.add_raygen_shader(cc::move(raygen));
    auto const miss_h = rpd.add_miss_shader(cc::move(miss));
    auto const hit_h = rpd.add_hit_shader({.closest_hit = cc::move(closest_hit)});
    auto rt_pipeline = ctx.uncached.create_raytracing_pipeline(rpd);
    REQUIRE(rt_pipeline != nullptr);

    auto stbd = sg::raytracing_shader_table_description{.pipeline = rt_pipeline};
    auto const raygen_idx = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(hit_h);
    auto rt_table = ctx.uncached.create_raytracing_shader_table(stbd);
    REQUIRE(rt_table != nullptr);

    // --- blit pipeline (fullscreen triangle sampling the ray-traced image) --------------------------
    auto blit_vs = compile(blit_hlsl, "main_vs", sg::shader_stage::vertex, ssc::dxc::shader_model::sm_6_8);
    auto blit_ps = compile(blit_hlsl, "main_ps", sg::shader_stage::fragment, ssc::dxc::shader_model::sm_6_8);

    auto blit_group_layout = ctx.uncached.create_binding_group_layout(blit_ps.bindings);
    REQUIRE(blit_group_layout != nullptr);
    auto blit_pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {blit_group_layout}});
    REQUIRE(blit_pipeline_layout != nullptr);

    auto blit_desc = sg::raster_pipeline_description{
        .layout = blit_pipeline_layout,
        .vertex_shader = cc::move(blit_vs),
        .fragment_shader = cc::move(blit_ps),
        .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = {{.format = sc->format()}},
    };
    auto blit_pipeline = ctx.uncached.create_raster_pipeline(blit_desc);
    REQUIRE(blit_pipeline != nullptr);

    auto const blit_sampler = sg::sampler{.min_filter = sg::sampler_filter::linear,
                                          .mag_filter = sg::sampler_filter::linear,
                                          .mip_filter = sg::sampler_filter::nearest,
                                          .address_u = sg::sampler_address_mode::clamp_edge,
                                          .address_v = sg::sampler_address_mode::clamp_edge};

    // --- cube BLAS (built once, reused every frame) -------------------------------------------------
    auto const cube = make_cube_positions();
    auto const vbuf = ctx.persistent.create_buffer<tg::pos3f>(
        cube.size(), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);

    sg::blas_handle blas;
    {
        auto up = ctx.create_command_list();
        up->upload.data_to_buffer(vbuf, cube);
        ctx.submit_command_list(cc::move(up));

        auto build = ctx.create_command_list();
        blas = build->raytracing.build_blas({{.vertices = vbuf.raw(), .vertex_count = cube.size()}});
        ctx.submit_command_list(cc::move(build));
        ctx.advance_epoch_and_wait_for_idle();
    }
    REQUIRE(blas != nullptr);

    // Per-frame camera constants; the contents are refreshed each frame, the buffer is not.
    auto const cam_buf
        = ctx.persistent.create_buffer<camera_data>(1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);

    auto const start = std::chrono::steady_clock::now();
    constexpr auto max_duration = std::chrono::seconds(30);

    while (pump_messages())
    {
        auto const elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > max_duration)
            break;
        auto const t = std::chrono::duration<float>(elapsed).count();

        auto rt = sc->acquire_backbuffer(); // auto-resizes to the window
        auto const size = rt.size();        // the acquired view is this frame's size of record

        // Offscreen ray-tracing target: UAV-written by the raygen, then sampled (SRV) by the blit.
        // Transient, so it follows the window size for free and expires with the epoch.
        auto const image = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba16_float, // UAV-storable + samplable
             .width = size[0],
             .height = size[1],
             .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

        // Fixed camera looking at the origin; only the cube spins (via the TLAS instance transform).
        auto const cam = make_camera(tg::vec3f(2.2f, 1.8f, -3.2f), rt.aspect_ratio(), 60_deg_f);
        auto const rot = tg::mat3f::make_rotation_y(t * 50_deg_f) * tg::mat3f::make_rotation_x(t * 30_deg_f);

        auto cmd = ctx.create_command_list();
        cmd->upload.pod_to_buffer(cam_buf, cam); // inline; read by the trace below

        // Rebuild the TLAS for this frame's transform (refit isn't implemented), then trace.
        sg::tlas_handle const tlas = cmd->raytracing.build_tlas({make_cube_instance(blas, rot)});
        auto rt_group = ctx.transient.create_binding_group(rt_group_layout,
                                                           {{.name = "scene", .view = tlas->as_view()},
                                                            {.name = "Output", .view = image.as_readwrite_view()},
                                                            {.name = "Camera", .view = cam_buf.as_uniform_buffer()}});
        cmd->raytracing.bind_pipeline(*rt_pipeline);
        cmd->raytracing.bind_group(0, *rt_group);
        cmd->raytracing.dispatch_rays(*rt_table, raygen_idx, size[0], size[1]);

        // Blit the ray-traced image onto the back buffer.
        auto blit_group
            = ctx.transient.create_binding_group(blit_group_layout, {{.name = "Src", .view = image.as_readonly_view()}},
                                                 {{.name = "Samp", .sampler = blit_sampler}});
        {
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0, 0, 0, 1))}});
            cmd->raster.bind_pipeline(*blit_pipeline);
            cmd->raster.bind_group(0, *blit_group);
            cmd->raster.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        ctx.submit_command_list_and_present(*sc, cc::move(cmd));
        ctx.advance_epoch(sc->buffer_count());
    }

    ctx.advance_epoch_and_wait_for_idle();
    if (!s_window_closed)
        DestroyWindow(hwnd);
    CHECK(true); // manual visual test — reaching here means the frame loop ran and tore down cleanly
}
