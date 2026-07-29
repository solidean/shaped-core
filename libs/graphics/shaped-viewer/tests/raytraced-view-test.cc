#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless flat-PBR raytrace on WARP (or a hardware device): build a random triangle cloud through the
// managers and dispatch one direct-lit ray per pixel via pbr_raytrace_routine, driven to completion. The
// view_renderer drives the path tracer now, so this keeps the simpler flat routine exercised directly — the
// slib-acquired ray-tracing shaders compile, the DXR pipeline + shader table build, the TLAS is built, and
// dispatch_rays runs.
//
// No pixel readback: texture download is not wired in sg yet, so this asserts the pipeline runs rather than
// inspecting the image. Reaching the end without an assert/exception means every GPU stage succeeded.
TEST("sv - flat-PBR raytraced view (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
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

    // Build the scene through the managers (this is where the BLAS is built).
    auto const cloud = sv_test::make_triangle_cloud(64);
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));
    REQUIRE(resources.meshes.contains(mesh));
    REQUIRE(resources.materials.contains(materials));

    auto const* const mesh_rec = resources.meshes.get_ptr(mesh);
    auto const* const mat_rec = resources.materials.get_ptr(materials);
    REQUIRE(mesh_rec != nullptr);
    REQUIRE(mat_rec != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0});

    auto const size = tg::vec2i(128, 128);
    auto const cam = sv::camera{
        .position = tg::pos3d(2.4, 1.8, -3.2)}; // default orientation frames the origin; square target -> aspect 1

    // Flat-PBR frame constants: the camera — the surfaces are lit entirely by the SH environment probe — plus
    // the bound mesh's geometry layout, which tells the closest-hit how to reach a triangle's vertices.
    auto fc = sv::frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(cam);
    fc.mesh_is_indexed = mesh_rec->is_indexed;

    // A simple SH environment: a bright DC term plus a vertical (y) gradient, so the surfaces get diffuse
    // irradiance (and a missed ray sees a sky).
    auto bg = sv::background{};
    bg.sh[0] = tg::vec3f(1.40f, 1.72f, 2.20f); // constant (DC) radiance — the ambient sky level
    bg.sh[1] = tg::vec3f(0.30f, 0.42f, 0.60f); // Y(1,-1): brighter overhead, darker below

    auto cmd = ctx.create_command_list();

    auto const frame = ctx.transient.create_buffer<sv::frame_constants_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.pod_to_buffer(frame, fc);

    auto const background = ctx.transient.create_buffer<sv::background_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.pod_to_buffer(background, sv::background_gpu::from(bg));

    auto const target = ctx.transient.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float, // UAV-writable by the raygen
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

    sv::pbr_raytrace_routine::execute(*cmd, {.frame = frame,
                                             .background = background,
                                             .instances = instances,
                                             .output = target,
                                             .materials = mat_rec->materials,
                                             .vertices = mesh_rec->vertices,
                                             .indices = mesh_rec->indices,
                                             .size = size});

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // Reaching here means the whole flat-PBR pipeline ran (BLAS + TLAS build, DXR dispatch) without a device error.
    CHECK(mesh_rec->triangle_count > 0);
    CHECK(!mesh_rec->is_indexed); // the non-indexed path: a non-indexed BLAS + the stand-in bound as Indices
}
