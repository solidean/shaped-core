#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless end-to-end path trace on WARP (or a hardware device).
// It builds a simple Cornell box through the managers, integrates one small view with global illumination, and drives it to completion.
// Beyond the flat direct-lit raytraced-view test, this exercises the whole GI path.
// The path-tracing shaders compile through slib, the DXR pipeline + shader table build, the TLAS is built, and the raygen bounces rays with NEE toward the ceiling light.
//
// No pixel readback: this asserts the pipeline runs rather than inspecting the image (same philosophy as the
// raytraced-view test). Reaching the end without an assert/exception means every GPU stage succeeded.
// On the main thread, because `pathtrace_routine::init_declare` drives its shader compiles inline through
// `try_async_blocking_get` — which does not complete from inside a pool worker, leaving the routine
// with no pipeline and `execute` silently doing nothing.
// Same reason shaped-rendering pins its dispatch test (commit 9c9c6ef7).
TEST("sv - path-traced Cornell box (headless)", nx::config::main_thread)
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

    // Build the Cornell box through the managers: the BLAS build, the material resolution and the permutation compile
    // all happen here, exactly as `scene_ref::add_mesh` drives them.
    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cornell box", box.positions, box.materials));
    REQUIRE(resources.meshes.contains(item.mesh));
    REQUIRE(resources.contains_instance(item.instance));

    // An acquire queues the upload and the BLAS build; a test tracing what it just built has no frame loop to drain
    // that queue, so it drains it itself.
    resources.wait_for_pending_uploads();

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);

    // Re-adding an unchanged mesh must land on every id it already minted, since that is what keeps a per-frame
    // add_mesh O(1) rather than an upload and a compile.
    auto const again = resources.acquire_scene_item(sv_test::as_mesh("cornell box", box.positions, box.materials));
    CHECK(again.mesh == item.mesh);
    CHECK(again.instance == item.instance);
    CHECK(again.shader_key == item.shader_key);

    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    // One instance at identity — the Cornell box geometry is already in world space.
    // Hit group 0 is the one permutation this scene shades with.
    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

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

    // A closed Cornell box lets no ray escape, so the environment probe stays dark; still bind it (the miss
    // reads it). Zero coefficients = black background.
    auto const bg = sv::background{};

    // Built on the list that traces with it: every bindless index it names is minted here, for this epoch.
    // Declared out here because the checks below read it back.
    auto records = cc::vector<sv::instance_gpu>();

    // The routine degrades to a no-op when its shaders do not build, so without this every CPU-side check below
    // still passes against a target nothing ever wrote.
    // That silence is expensive: a shader break shows up as a debugging session on the image, not a failing test.
    //
    // Driven as whole frames rather than asserted on one: the trace additionally waits on a DXR state object that is
    // built asynchronously and polled across frames — see sv_test::frames_until_executed.
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            records.clear();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

            auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(frame, fc);

            auto const background = ctx.transient.create_buffer<sv::background_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(background, sv::background_gpu::from(bg));

            // rgba32_float, which the routine asserts on: the raygen reads the target back to blend into it.
            auto const target = ctx.transient.create_texture_2d(
                {.format = sg::pixel_format::rgba32_float,
                 .width = size[0],
                 .height = size[1],
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

            // One `sv::instance` per TLAS instance: where this item's material parameters live, and its geometry.
            auto const instance_table = ctx.transient.create_buffer<sv::instance_gpu>(
                records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.data_to_buffer(instance_table, records);

            // The tables the closest-hit reaches all of that through, locked for the recording.
            auto const bindless = resources.freeze();

            return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = target,
                                                        .instance_table = instance_table,
                                                        .hit_groups = hit_groups,
                                                        .bindless = &bindless});
        }));

    // Reaching here means the whole GI pipeline ran (BLAS + TLAS build, DXR dispatch) without a device error.
    CHECK(mesh_rec->triangle_count == box.materials.size());
    CHECK(!mesh_rec->is_indexed); // the non-indexed path: the corner indices come from the primitive, not a buffer
    CHECK(records[0].is_indexed == 0u);
}

TEST("sv::pathtrace_routine - a material that does not compile costs its own meshes, not the view")
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

    // A material type whose fragment is not HLSL, which is the case the fallback exists for: before it, one of these
    // anywhere in a scene made the whole trace a no-op.
    auto& lib = *sv::acquire_material_library().value();
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    auto const type = lib.register_type(
        sv::material_type::create("sv_test_broken", cc::move(signature), "    surface.x = not_a_function(roughness);"));

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto mesh = sv_test::as_mesh("broken", box.positions, box.materials);
    mesh.material = lib.acquire(sv::material::create("sv_test_broken", type, {}));

    auto const item = resources.acquire_scene_item(mesh);
    resources.wait_for_pending_uploads();

    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0, .hit_group_offset = 0});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto const size = tg::vec2i(16, 16); // nothing here reads the image, so it is as small as a dispatch can be

    // Records one trace into `cmd` and reports what it decided; the caller owns the frame around it, so the same body
    // serves a one-shot check and a driven one.
    auto const trace = [&](sg::command_list& cmd, sv::material_permutation const* fallback)
    {
        auto records = cc::vector<sv::instance_gpu>();
        records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

        auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
            1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(frame, sv::pt_frame_constants_gpu{.samples_per_pixel = 1, .max_bounces = 1});

        auto const background = ctx.transient.create_buffer<sv::background_gpu>(
            1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(background, sv::background_gpu::from(sv::background{}));

        auto const target = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba32_float,
             .width = size[0],
             .height = size[1],
             .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

        auto const instance_table = ctx.transient.create_buffer<sv::instance_gpu>(
            records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.data_to_buffer(instance_table, records);

        auto const bindless = resources.freeze();
        return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                    .background = background,
                                                    .instances = instances,
                                                    .output = target,
                                                    .instance_table = instance_table,
                                                    .hit_groups = hit_groups,
                                                    .fallback = fallback,
                                                    .bindless = &bindless});
    };

    // The permutation genuinely does not build, so it cannot be traced with.
    (void)cc::try_async_blocking_get(permutation->shader);
    REQUIRE(permutation->shader->has_error());

    // With nothing to stand in for it the trace is a no-op — the old all-or-nothing behavior, still what a caller
    // supplying no fallback gets.
    // One frame is enough for a decline, and it must stay a decline however long anything else takes.
    {
        auto cmd = ctx.create_command_list();
        CHECK(trace(*cmd, nullptr) == sg::routine_outcome::declined);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        ctx.block_until_idle();
    }

    // With the neutral hit group it dispatches: the mesh is placed and shaded grey rather than the view going dark.
    // Driven, because the fallback's own state object is built asynchronously — see sv_test::frames_until_executed.
    CHECK(sv_test::frames_until_executed(
        ctx, [&](sg::command_list& cmd) { return trace(cmd, &resources.shaders.acquire_fallback()); }));
}

// The same trace, shaded through a texture rather than through per-face colours.
//
// A permutation declares a sampler only when its material samples something, and those samplers are a group of
// their own -- a third `binding_group_layout` and a third slot in the pipeline layout (see
// `sv::material_sampler_group`).
// Every other path-traced scene in this suite is untextured, so without this the three-way split in
// `_build_variant` and the layout it builds are never reached at all.
//
// What makes this test mean something is the sampler count below: a change that stopped generating samplers would
// otherwise leave it green while testing nothing.
TEST("sv - a path-traced textured material builds its sampler group (headless)", nx::config::main_thread)
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

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_textured_mesh("textured box", box.positions));
    REQUIRE(resources.meshes.contains(item.mesh));
    resources.wait_for_pending_uploads();

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);

    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    // The guard this test rests on: a textured material is what puts a sampler in the permutation, and a sampler
    // is what puts a third group in the pipeline layout.
    REQUIRE(permutation->samplers.size() >= 1);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto const size = tg::vec2i(64, 64);
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    auto fc = sv::pt_frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(cam);
    fc.light = {.center = box.light.center,
                .u = tg::vec3f(box.light.half_x, 0, 0),
                .v = tg::vec3f(0, 0, box.light.half_z),
                .emission = box.light.emission,
                .normal = tg::vec3f(0, -1, 0)};
    fc.samples_per_pixel = 4;
    fc.max_bounces = 3;
    fc.seed = 1u;

    // PERSISTENT rather than transient: the loop below advances an epoch per frame, which expires a transient one.
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

    // A root signature the sampler group broke would fail pipeline creation, and the routine would degrade to a
    // no-op rather than say so -- which is exactly what this REQUIRE is here to stop.
    //
    // Driven over frames rather than asserted on the first one: the permutation's own closest-hit still has to
    // compile and the DXR state object is built across frames, so "traced within one frame" would be a statement
    // about the machine rather than about the layout.
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            auto records = cc::vector<sv::instance_gpu>();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

            auto const frame = ctx.transient.create_buffer<sv::pt_frame_constants_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(frame, fc);

            auto const background = ctx.transient.create_buffer<sv::background_gpu>(
                1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(background, sv::background_gpu::from(sv::background{}));

            auto const instance_table = ctx.transient.create_buffer<sv::instance_gpu>(
                records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.data_to_buffer(instance_table, records);

            auto const bindless = resources.freeze();

            return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = target,
                                                        .instance_table = instance_table,
                                                        .hit_groups = hit_groups,
                                                        .bindless = &bindless});
        }));
}
