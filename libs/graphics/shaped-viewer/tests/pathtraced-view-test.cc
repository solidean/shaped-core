#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless end-to-end path-tracing trace on WARP (or a hardware device): build a simple Cornell box through
// the managers, integrate one small view with global illumination, and drive it to completion. Beyond the
// flat direct-lit raytraced-view test, this exercises the whole GI path — the path-tracing shaders compile
// through slib, the DXR pipeline + shader table build, the TLAS is built, and the raygen bounces rays with
// next-event estimation toward the ceiling light.
//
// No pixel readback: this asserts the pipeline runs rather than inspecting the image (same philosophy as the
// raytraced-view test). Reaching the end without an assert/exception means every GPU stage succeeded.
TEST("sv - path-traced Cornell box (headless)")
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
        SKIP("no DXC compiler to build the path-tracing shaders");

    // Build the Cornell box through the managers (this is where the BLAS is built).
    auto const box = sv_test::make_cornell_box();
    auto resources = sv::scene_resources::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(box.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(box.materials));
    REQUIRE(resources.meshes.contains(mesh));
    REQUIRE(resources.materials.contains(materials));

    auto const* const mesh_rec = resources.meshes.get_ptr(mesh);
    auto const* const mat_rec = resources.materials.get_ptr(materials);
    REQUIRE(mesh_rec != nullptr);
    REQUIRE(mat_rec != nullptr);

    // One instance at identity — the Cornell box geometry is already in world space.
    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0});

    // A pinhole camera outside the open front, looking down the +z axis into the box.
    auto const size = tg::vec2i(96, 96);
    auto cam = sv::camera{.position
                          = tg::pos3d(0, 0, -3.4)}; // default orientation frames the origin; square target -> aspect 1
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    // Frame constants: camera + the same light rectangle the geometry emits from + modest sample controls
    // (kept small so the trace stays fast on the WARP software device).
    auto fc = sv::pt_frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(cam);
    // the box light is an axis-aligned XZ rect, emitting straight down
    fc.light = {.center = box.light.center,
                .u = tg::vec3f(box.light.half_x, 0, 0),
                .v = tg::vec3f(0, 0, box.light.half_z),
                .emission = box.light.emission,
                .normal = tg::vec3f(0, -1, 0)};
    fc.samples_per_pixel = 16;
    fc.max_bounces = 5;
    fc.seed = 1u;
    fc.mesh_is_indexed = mesh_rec->is_indexed; // a plain triangle list here — the closest-hit skips Indices

    // A closed Cornell box lets no ray escape, so the environment probe stays dark; still bind it (the miss
    // reads it). Zero coefficients = black background.
    auto const bg = sv::background{};

    auto cmd = ctx.create_command_list();

    auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
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

    sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                          .background = background,
                                          .instances = instances,
                                          .output = target,
                                          .materials = mat_rec->materials,
                                          .vertices = mesh_rec->vertices,
                                          .indices = mesh_rec->indices});

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // Reaching here means the whole GI pipeline ran (BLAS + TLAS build, DXR dispatch) without a device error.
    CHECK(mesh_rec->triangle_count == box.materials.size());
    CHECK(!mesh_rec->is_indexed); // the non-indexed path: a non-indexed BLAS + the stand-in bound as Indices
}
