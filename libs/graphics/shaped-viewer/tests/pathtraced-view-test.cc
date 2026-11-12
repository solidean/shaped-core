#include "viewer_test_env.hh"

#include <clean-core/container/tuple.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>
#include <typed-geometry/scalar/half_float.hh>

using namespace cc::primitive_defines;

// Headless end-to-end path trace.
// It builds a simple Cornell box through the managers, integrates one small view with global illumination, and drives it to completion.
// The path-tracing shaders compile through slib, the ray-tracing pipeline and shader table build, the TLAS is built, and the raygen bounces rays with NEE toward the ceiling light.
//
// No pixel readback: this asserts the pipeline runs rather than inspecting the image.
// Reaching the end without an assert or an exception means every GPU stage succeeded.
ASYNC_INVOCABLE_TEST("sv - path-traced Cornell box (headless)", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");

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
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    // A pinhole camera outside the open front, looking down the +z axis into the box.
    auto const size = tg::vec2i(96, 96);
    auto cam = sv::camera{.position
                          = tg::pos3d(0, 0, -3.4)}; // default orientation frames the origin; square target -> aspect 1
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    // Frame constants: camera + the same light rectangle the geometry emits from + modest sample controls
    // (kept small so the trace stays cheap on the WARP software device).
    auto fc = sv::default_frame_constants();
    fc.camera = sv::camera_record_of(cam);
    // the box light is an axis-aligned XZ rect, emitting straight down
    auto const lights = sv_test::light_table_of(box.light);
    lights.describe_in(fc);
    fc.samples_per_pixel = 16;
    fc.max_bounces = 5;
    fc.rng_seed = 1u;

    // A closed Cornell box lets no ray escape, so the environment probe stays dark; still bind it (the miss
    // reads it). Zero coefficients = black background.
    auto const bg = sv::background{};

    // Built on the list that traces with it: every bindless index it names is minted here, for this epoch.
    // Declared out here because the checks below read it back.
    auto records = cc::vector<sv::shaders::tracer::instance_record>();

    // The routine degrades to a no-op when its shaders do not build, so without this every CPU-side check below
    // still passes against a target nothing ever wrote.
    // That silence is expensive: a shader break shows up as a debugging session on the image, not a failing test.
    //
    // Driven as whole frames rather than asserted on one: the trace additionally waits on a ray-tracing state object that is
    // built asynchronously and polled across frames — see sv_test::frames_until_executed.
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            records.clear();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

            auto const frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);

            auto const background = ctx.transient.create_buffer_from_pod(cmd, sv::background_gpu::from(bg),
                                                                         sg::buffer_usage::readonly_buffer);

            // rgba32_float, which the routine asserts on: the raygen reads the target back to blend into it.
            auto const target
                = ctx.transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                   .width = size[0],
                                                   .height = size[1],
                                                   .usage = sg::texture_usage::texture | sg::texture_usage::image});

            // One `sv::instance` per TLAS instance: where this item's material parameters live, and its geometry.
            auto const instance_table
                = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);
            auto const light_buffer = sv_test::upload_lights(cmd, lights);

            // The tables the closest-hit reaches all of that through, locked for the recording.
            auto const bindless = resources.freeze();

            return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = target,
                                                        .instance_table = instance_table,
                                                        .lights = light_buffer,
                                                        .hit_groups = hit_groups,
                                                        .bindless = &bindless});
        }));

    // Reaching here means the whole GI pipeline ran (BLAS + TLAS build, ray dispatch) without a device error.
    CHECK(mesh_rec->geometry.triangle_count == box.materials.size());
    // A raw list is indexed in the pool too: one vertex per corner.
    CHECK(mesh_rec->geometry.vertex_count == mesh_rec->geometry.index_count());
    CHECK(records[0].first_index == u32(mesh_rec->geometry.first_index));

    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv::pathtrace_routine - a material that does not compile costs its own meshes, not the view",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");

    // A material type whose fragment does not compile, which is the case the fallback exists for: before it, one of these
    // anywhere in a scene made the whole trace a no-op.
    auto& lib = *sv::acquire_material_library().value();
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    auto const type = lib.register_type(sv::material_type::create(
        {.name = "sv_test_broken", .signature = cc::move(signature), .shader = "surface.x = not_a_function(roughness)\n"}));

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
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto const size = tg::vec2i(16, 16); // nothing here reads the image, so it is as small as a dispatch can be

    // Records one trace into `cmd` and reports what it decided; the caller owns the frame around it, so the same body
    // serves a one-shot check and a driven one.
    auto stand_ins = sv::stand_in_report{};
    auto const trace = [&](sg::command_list& cmd, sv::material_permutation const* fallback)
    {
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

        auto const frame = ctx.transient.create_buffer_from_pod(
            cmd, sv::shaders::tracer::frame_constants{.samples_per_pixel = 1, .max_bounces = 1, .rng_seed = 1},
            sg::buffer_usage::readonly_buffer);

        auto const background = ctx.transient.create_buffer_from_pod(cmd, sv::background_gpu::from(sv::background{}),
                                                                     sg::buffer_usage::readonly_buffer);

        auto const target
            = ctx.transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                               .width = size[0],
                                               .height = size[1],
                                               .usage = sg::texture_usage::texture | sg::texture_usage::image});

        auto const instance_table
            = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);

        auto const bindless = resources.freeze();
        return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                    .background = background,
                                                    .instances = instances,
                                                    .output = target,
                                                    .instance_table = instance_table,
                                                    .hit_groups = hit_groups,
                                                    .fallback = fallback,
                                                    .bindless = &bindless,
                                                    .stand_ins = &stand_ins});
    };

    // The permutation genuinely does not build, so it cannot be traced with.
    co_await cc::async_settled(permutation->hit_group);
    REQUIRE(permutation->hit_group->has_error());

    // The trace says why the mesh shades as the stand-in, naming the material and the compiler's own message.
    // Once, however many frames trace it: the routine remembers that it said so.
    nx::expect_error("the hit group of <material 'sv_test_broken'> did not compile*not_a_function*", nx::exactly(1));

    // With nothing to stand in for it the trace is a no-op — the old all-or-nothing behavior, still what a caller
    // supplying no fallback gets.
    // One frame is enough for a decline, and it must stay a decline however long anything else takes.
    {
        auto cmd = ctx.create_command_list();
        CHECK(trace(*cmd, nullptr) == sg::routine_outcome::declined);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }

    // With the neutral hit group it dispatches: the mesh is placed and shaded grey rather than the view going dark.
    // Driven, because the fallback's own state object is built asynchronously — see sv_test::frames_until_executed.
    CHECK(sv_test::frames_until_executed(
        ctx, [&](sg::command_list& cmd) { return trace(cmd, &resources.shaders.acquire_fallback()); }));

    // And it says so: a frame shaded by the stand-in is an image of other materials, which a mean must tell apart.
    // The compile failed for good, so the substitution is settled and a mean of it may converge.
    CHECK(stand_ins.hash != 0);
    CHECK(!stand_ins.pending);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The same trace, shaded through a texture rather than through per-face colours.
//
// A permutation samples only when its material does, through module tracer's palette in the bindless group.
// Every other path-traced scene in this suite is untextured, so without this a hit group reaching the palette and the
// bindless texture table is never traced at all.
//
// What makes this test mean something is the palette check below: a change that stopped generating samples would
// otherwise leave it green while testing nothing.
ASYNC_INVOCABLE_TEST("sv - a path-traced textured material samples through the palette (headless)",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_textured_mesh("textured box", box.positions));
    REQUIRE(resources.meshes.contains(item.mesh));
    resources.wait_for_pending_uploads();

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);

    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    // The guard this test rests on: a textured material is what puts a sample in the permutation.
    REQUIRE(permutation->source.contains("tracer.bindless.palette_"));

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto const size = tg::vec2i(64, 64);
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    auto fc = sv::default_frame_constants();
    fc.camera = sv::camera_record_of(cam);
    // the box light is an axis-aligned XZ rect, emitting straight down
    auto const lights = sv_test::light_table_of(box.light);
    lights.describe_in(fc);
    fc.samples_per_pixel = 4;
    fc.max_bounces = 3;
    fc.rng_seed = 1u;

    // PERSISTENT rather than transient: the loop below advances an epoch per frame, which expires a transient one.
    auto const target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::texture | sg::texture_usage::image});

    // A hit group that did not build would degrade the routine to a no-op rather than say so -- which is exactly what
    // this REQUIRE is here to stop.
    //
    // Driven over frames rather than asserted on the first one: the permutation's own hit group still has to compile
    // and the state object is built across frames, so "traced within one frame" would be a statement about the
    // machine rather than about the layout.
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

            auto const frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);

            auto const background = ctx.transient.create_buffer_from_pod(
                cmd, sv::background_gpu::from(sv::background{}), sg::buffer_usage::readonly_buffer);

            auto const instance_table
                = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);
            auto const light_buffer = sv_test::upload_lights(cmd, lights);

            auto const bindless = resources.freeze();

            return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = target,
                                                        .instance_table = instance_table,
                                                        .lights = light_buffer,
                                                        .hit_groups = hit_groups,
                                                        .bindless = &bindless});
        }));

    co_await cc::async_settled(sv::background_work(ctx));
}

// The split signals add back to the frame they were split from.
//
// A split-signal denoiser filters the diffuse and specular halves with different kernels and sums them again, so the
// sum has to be the picture the tracer would have produced unsplit.
// If it is not, every image the denoiser produces is wrong by whatever the split lost — a bias that looks like the
// denoiser being slightly off and that no amount of convergence removes.
//
// The raygen keeps the total exact and tracks only the specular SHARE, writing the diffuse half as the difference, so
// this holds by construction rather than by two accumulations happening to agree.
// The test is what stops that construction being quietly replaced by two.
ASYNC_INVOCABLE_TEST("sv::pathtrace_routine - the split signals sum to the frame", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cornell box", box.positions, box.materials));
    resources.wait_for_pending_uploads();

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto const size = tg::vec2i(48, 48);
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    auto fc = sv::default_frame_constants();
    fc.camera = sv::camera_record_of(cam);
    auto const lights = sv_test::light_table_of(box.light);
    lights.describe_in(fc);
    fc.samples_per_pixel = 4;
    fc.max_bounces = 4;
    fc.rng_seed = 1u;
    fc.write_temporal = 1;
    fc.write_split = 1;

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format,
             .width = size[0],
             .height = size[1],
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
    };

    // The formats the viewer allocates: each signal is rounded to a half on its own, so the sum is held to that.
    auto const total = make(sg::pixel_format::rgba16_float);
    auto const diffuse = make(sg::pixel_format::rgba16_float);
    auto const specular = make(sg::pixel_format::rgba16_float);
    auto const motion = make(sg::pixel_format::rg32_float);
    auto const hit_distance = make(sg::pixel_format::rg32_float);
    auto const accumulator = make(sg::pixel_format::rgba32_float);

    auto records = cc::vector<sv::shaders::tracer::instance_record>();
    REQUIRE(sv_test::frames_until_executed(
        ctx,
        [&](sg::command_list& cmd)
        {
            records.clear();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));

            auto const frame = ctx.transient.create_buffer<sv::shaders::tracer::frame_constants>(
                1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(frame, fc);

            auto const background = ctx.transient.create_buffer<sv::background_gpu>(
                1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.pod_to_buffer(background, sv::background_gpu::from(sv::background{}));

            auto const instance_table = ctx.transient.create_buffer<sv::shaders::tracer::instance_record>(
                records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
            cmd.upload.data_to_buffer(instance_table, records);
            auto const light_buffer = sv_test::upload_lights(cmd, lights);

            auto const bindless = resources.freeze();
            return sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                        .background = background,
                                                        .instances = instances,
                                                        .output = accumulator,
                                                        .frame_output = total,
                                                        .guide_motion = motion,
                                                        .frame_diffuse = diffuse,
                                                        .frame_specular = specular,
                                                        .guide_hit_distance = hit_distance,
                                                        .instance_table = instance_table,
                                                        .lights = light_buffer,
                                                        .hit_groups = hit_groups,
                                                        .bindless = &bindless});
        }));

    auto cmd = ctx.create_command_list();
    auto total_back = sg::data_future<cc::fixed_array<u16, 4>>(cmd->download.bytes_from_texture(total.raw()));
    auto diffuse_back = sg::data_future<cc::fixed_array<u16, 4>>(cmd->download.bytes_from_texture(diffuse.raw()));
    auto specular_back = sg::data_future<cc::fixed_array<u16, 4>>(cmd->download.bytes_from_texture(specular.raw()));
    auto hit_back = sg::data_future<tg::vec2f>(cmd->download.bytes_from_texture(hit_distance.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const widened = [](cc::span<cc::fixed_array<u16, 4> const> halves)
    {
        auto out = cc::vector<tg::vec4f>();
        for (auto const& p : halves)
            out.push_back(tg::vec4f(f32(tg::half_float::make_from_bits(p[0])), f32(tg::half_float::make_from_bits(p[1])),
                                    f32(tg::half_float::make_from_bits(p[2])), f32(tg::half_float::make_from_bits(p[3]))));
        return out;
    };
    auto const t = widened(co_await total_back.data());
    auto const d = widened(co_await diffuse_back.data());
    auto const s = widened(co_await specular_back.data());
    auto const h = co_await hit_back.data();
    REQUIRE(t.size() == size[0] * size[1]);
    REQUIRE(h.size() == t.size());

    auto worst = 0.0f;
    auto any_specular = false;
    auto any_diffuse = false;
    auto negative = false;
    for (auto i = isize(0); i < t.size(); ++i)
        for (auto c = 0; c < 3; ++c)
        {
            // three halves, each within half a step of 2^-10 relative, so their sum within 3 * 2^-11 of the largest
            auto const scale = cc::max(cc::max(tg::abs(t[i][c]), tg::abs(d[i][c]) + tg::abs(s[i][c])), 1e-3f);
            worst = cc::max(worst, tg::abs((d[i][c] + s[i][c]) - t[i][c]) / scale);
            any_specular |= s[i][c] > 1e-4f;
            any_diffuse |= d[i][c] > 1e-4f;

            // Neither half may go negative: they are radiance, and a denoiser handed a negative signal produces holes.
            negative |= d[i][c] < -1e-5f || s[i][c] < -1e-5f;
        }

    CHECK(worst <= 3.0f / 2048.0f).context(cc::format("worst |diffuse + specular - total|, relative, was {}", worst));
    CHECK(!negative);

    // And the split is doing something: a Cornell box lit through a specular-and-diffuse closure has both.
    // Without this the sum check passes trivially on an all-diffuse image.
    CHECK(any_diffuse);
    CHECK(any_specular);

    // The hit distances are what pin the path CLASSIFICATION, which the two checks above do not.
    // A specular highlight reaches the specular half through direct lighting alone, so forcing every path to be
    // classified diffuse still leaves `any_specular` true — it was the first version of this test, and it passed.
    // Only a path that continued through a specular lobe writes a specular hit distance.
    auto diffuse_bounced = false;
    auto specular_bounced = false;
    for (auto const& p : h)
    {
        diffuse_bounced |= p[0] > 1e-4f;
        specular_bounced |= p[1] > 1e-4f;
    }
    CHECK(diffuse_bounced).context("no path was classified diffuse and went on to hit something");
    CHECK(specular_bounced).context("no path was classified specular and went on to hit something");

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// One mesh at identity, seen by one camera — what the light tests below trace under different lights.
struct light_scene
{
    sv::scene_item item;
    cc::vector<sg::tlas_instance> instances;
    cc::vector<sv::material_permutation const*> hit_groups;
    sv::camera camera;
    i32 size = 48;
    i32 samples_per_pixel = 64;
    i32 max_bounces = 3;

    /// what an escaping ray sees; black unless a test asks for a sky
    sv::background environment = {};
};

/// `placed`, acquired and ready to trace, or empty when it did not build.
[[nodiscard]] cc::optional<light_scene> make_light_scene(sv::gpu_resource_manager& resources,
                                                         sv::mesh const& placed,
                                                         sv::camera const& camera)
{
    auto scene = light_scene{};
    scene.item = resources.acquire_scene_item(placed);
    resources.wait_for_pending_uploads();

    auto const* const mesh = resources.meshes.get_ptr(scene.item.mesh);
    auto const* const permutation = resources.shaders.find(scene.item.shader_key);
    if (mesh == nullptr || permutation == nullptr)
        return {};

    scene.instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, scene.item.mesh), .instance_id = 0, .hit_group_offset = 0});
    scene.hit_groups.push_back(permutation);
    scene.camera = camera;
    return scene;
}

/// The mesh `positions` / `materials` describe, acquired and ready to trace, or empty when it did not build.
[[nodiscard]] cc::optional<light_scene> make_light_scene(sv::gpu_resource_manager& resources,
                                                         cc::vector<tg::pos3f> const& positions,
                                                         cc::vector<sv_test::pbr_material> const& materials,
                                                         sv::camera const& camera)
{
    return make_light_scene(resources, sv_test::as_mesh("light scene", positions, materials), camera);
}

/// Traces `scene` once per table — all of them in one frame, with one seed, so the images differ only by their lights —
/// and hands back each image's pixels.
///
/// Empty when the trace never ran before the deadline, which the caller turns into a failure; nothing here asserts, since
/// a throw inside a coroutine would short-circuit the test rather than report.
/// Parameters by value, as a coroutine's must be: a reference would dangle across a suspend.
cc::shared_async<cc::vector<cc::vector<tg::vec4f>>> trace_under(sg::context* ctx,
                                                                sv::gpu_resource_manager* resources,
                                                                light_scene scene,
                                                                cc::vector<sv::pt_light_table> tables)
{
    auto images = cc::vector<cc::vector<tg::vec4f>>();
    auto const loop_start = cc::current_time_steady_secs();
    while (images.empty())
    {
        (void)ctx->routines.tick();

        auto readbacks = cc::vector<sg::data_future<tg::vec4f>>();
        auto all_executed = true;
        auto cmd = ctx->create_command_list();
        for (auto const& lights : tables)
        {
            auto fc = sv::default_frame_constants();
            fc.camera = sv::camera_record_of(scene.camera);
            // A pixel's footprint, as a view sets it, so a hit's textures pick the level its ray cone covers.
            sv::set_pixel_footprint(fc, scene.size);
            lights.describe_in(fc);
            fc.samples_per_pixel = scene.samples_per_pixel;
            fc.max_bounces = scene.max_bounces;
            fc.rng_seed = 1u;

            auto const frame = ctx->transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer);

            auto const background = ctx->transient.create_buffer_from_pod(
                *cmd, sv::background_gpu::from(scene.environment), sg::buffer_usage::readonly_buffer);

            auto const target = ctx->transient.create_texture_2d(
                {.format = sg::pixel_format::rgba32_float,
                 .width = scene.size,
                 .height = scene.size,
                 .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources->describe_instance(*cmd, scene.item.mesh, scene.item.instance));
            auto const instance_table
                = ctx->transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);

            // A table with no lights binds nothing, which is the routine's own stand-in path.
            auto const light_buffer = lights.records.empty() ? sg::buffer<sv::shaders::tracer::light_record>()
                                                             : sv_test::upload_lights(*cmd, lights);

            auto const bindless = resources->freeze();
            auto const outcome = sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                                                       .background = background,
                                                                       .instances = scene.instances,
                                                                       .output = target,
                                                                       .instance_table = instance_table,
                                                                       .lights = light_buffer,
                                                                       .hit_groups = scene.hit_groups,
                                                                       .bindless = &bindless});
            if (outcome != sg::routine_outcome::executed)
            {
                all_executed = false;
                break;
            }
            readbacks.push_back(sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw())));
        }
        ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->idle_completion();

        if (!all_executed)
        {
            // Not a failure yet — the state object has not landed; the deadline turns a hang into a message.
            if (cc::current_time_steady_secs() - loop_start > 45.0)
                co_return images;
            sv_test::drive_ambient_work();
            continue;
        }

        co_await ctx->idle_completion(); // an epoch advance drains the GPU but not the readback actor
        for (auto& readback : readbacks)
        {
            auto const delivered = readback.try_get_data();
            if (!delivered.has_value())
            {
                images.clear();
                co_return images;
            }
            auto pixels = cc::vector<tg::vec4f>();
            for (auto const& px : delivered.value().span())
                pixels.push_back(px);
            images.push_back(cc::move(pixels));
        }
    }
    co_return images;
}

/// The mean brightness of `image` over the square of `half` pixels either side of its center, or of all of it.
[[nodiscard]] float mean_brightness(cc::vector<tg::vec4f> const& image, i32 size, i32 half = -1)
{
    auto const lo = half < 0 ? 0 : size / 2 - half;
    auto const hi = half < 0 ? size : size / 2 + half;
    auto sum = 0.0;
    auto n = 0;
    for (auto y = lo; y < hi; ++y)
        for (auto x = lo; x < hi; ++x)
        {
            auto const& px = image[y * size + x];
            sum += (px[0] + px[1] + px[2]) / 3.0;
            ++n;
        }
    return float(sum / double(n));
}

/// The table of exactly `lights`.
[[nodiscard]] sv::pt_light_table table_of(cc::span<sv::light const> lights)
{
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    for (auto const& l : lights)
        records.push_back(sv::light_record_of(l));
    return sv::pt_light_table::grouped(records);
}

/// Whether this device and this build can trace at all; the reason to skip when not.
[[nodiscard]] cc::optional<cc::string_view> cannot_trace(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported();
    ctx.drop_command_list(cc::move(probe));
    if (!supported)
        return cc::string_view("device reports no ray tracing support");
    if (!sv_test::shared_env().has_compiler)
        return cc::string_view("no SGL compiler for this backend to build the path-tracing shaders");
    return {};
}
} // namespace

// Light is additive, so tracing every light together must give the sum of tracing each alone, less what none of them adds.
//
// That sum is what N lights have to get right and one light never exercised: the probability of picking a light has to
// be in its density, a delta light has to be divided by that probability alone, and the bounce ray has to credit every
// rect it crosses and every sun it escapes into.
// One of each path, so the pick is a quarter and every branch of the estimator runs; the sun is wide, so its two
// strategies both carry weight rather than next-event estimation taking all of it.
// Leave the pick out of any one density, or let the two strategies treat a light differently, and the sum is off by a
// clear factor rather than by noise.
ASYNC_INVOCABLE_TEST("sv - every kind of light traces to the sum of each alone", (sg::context_handle const& ctx_h))
{
    // The same inline-readback path volumetric-furnace-test documents as fastfailing on Windows on ARM.
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const scene = make_light_scene(resources, box.positions, box.materials, cam);
    REQUIRE(scene.has_value());

    // The box is closed but for its front, so the two distant lights come in through that opening, travelling +z and down.
    auto const inward = tg::vec3f(0, -0.35f, 1);
    auto const lights = cc::vector<sv::light>{
        sv::light::rect(tg::pos3f(-0.5f, 0.9f, 0), tg::vec3f(0.2f, 0, 0), tg::vec3f(0, 0, 0.2f)).nits(10),
        sv::light::spot(tg::pos3f(0.4f, 0.6f, -0.3f), tg::vec3f(0, -1, 0.3f), 40_deg_f, 25_deg_f).candela(1.5f),
        sv::light::directional(inward).lux(0.6f),
        sv::light::sun(inward, 30_deg_f).lux(0.6f),
    };

    // none, each alone, then all four
    auto tables = cc::vector<sv::pt_light_table>();
    tables.push_back(table_of({}));
    for (auto const& l : lights)
        tables.push_back(table_of(cc::span<sv::light const>(&l, 1)));
    tables.push_back(table_of(lights));

    // Held in a local: co_await hands back a reference into the async's node, which a temporary would take with it.
    auto const traced = trace_under(&ctx, &resources, scene.value(), tables);
    auto const& images = co_await traced;
    REQUIRE(images.size() == tables.size());

    auto const none = mean_brightness(images[0], scene.value().size);
    auto sum_alone = 0.0f;
    for (auto i = 1; i <= 4; ++i)
    {
        auto const added = mean_brightness(images[i], scene.value().size) - none;

        // Each light has to add something well clear of the noise, or the comparison below compares noise with noise.
        CHECK(added > 0.01f);
        sum_alone += added;
    }
    auto const added_all = mean_brightness(images[5], scene.value().size) - none;

    // Additive to within the sampling noise of six 64-sample images.
    CHECK(tg::abs(added_all - sum_alone) < 0.03f * sum_alone);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A light's unit means the same thing whatever its path: the illuminance a flat floor receives straight under it.
//
// So a parallel light of E lux, a sun of E lux, a point E * d^2 candela above the floor and a rect of the same candela
// all light the floor under them identically — each converted by a different formula on the CPU and read by a different
// branch of the estimator, which is what this pins.
// One bounce, so what the floor shows is those four numbers and nothing else.
// It is also what pins the path-length limit: the last hit has no bounce ray to share a rect's or a sun's weight with, so
// next-event estimation has to take all of it — before it did, a wide sun came out several percent dark here.
ASYNC_INVOCABLE_TEST("sv - every kind of light delivers the illuminance its unit promises",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    // A white floor at y = 0, seen from above.
    auto floor = sv_test::cornell_box{};
    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto scene = make_light_scene(resources, floor.positions, floor.materials, cam);
    REQUIRE(scene.has_value());
    scene.value().max_bounces = 1;

    auto const down = tg::vec3f(0, -1, 0);
    auto const lux = 2.0f;
    auto const height = 2.0f;
    auto const lights = cc::vector<sv::light>{
        sv::light::directional(down).lux(lux),
        sv::light::sun(down, 30_deg_f).lux(lux),
        sv::light::point(tg::pos3f(0, height, 0)).candela(lux * height * height),
        sv::light::rect(tg::pos3f(0, height, 0), tg::vec3f(0.05f, 0, 0), tg::vec3f(0, 0, 0.05f))
            .candela(lux * height * height),
    };

    auto tables = cc::vector<sv::pt_light_table>();
    for (auto const& l : lights)
        tables.push_back(table_of(cc::span<sv::light const>(&l, 1)));

    // Held in a local: co_await hands back a reference into the async's node, which a temporary would take with it.
    auto const traced = trace_under(&ctx, &resources, scene.value(), tables);
    auto const& images = co_await traced;
    REQUIRE(images.size() == tables.size());

    // The floor right under the lights, where a point's inverse square and cosine barely vary.
    auto const size = scene.value().size;
    auto const reference = mean_brightness(images[0], size, 2);
    CHECK(reference > 0.1f);
    for (auto i = 1; i < 4; ++i)
        CHECK(tg::abs(mean_brightness(images[i], size, 2) - reference) < 0.03f * reference);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The same illuminance with a bounce ray in flight, for the sun, the one path whose bounce branch can escape into it.
//
// At one bounce the ray that would reach the sun is never traced, so the test above pins next-event estimation alone.
// Here the floor is alone under a black sky, so a bounce off it can only escape — into the disc or into nothing — and
// the floor's radiance is still exactly the sun's direct light, split between the two strategies.
// The parallel light beside it has no bounce branch at all, which is what makes the comparison absolute: an error in the
// sun's bounce weighting does not cancel against anything.
ASYNC_INVOCABLE_TEST("sv - a sun delivers its illuminance with its bounce-ray strategy in play",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    auto floor = sv_test::cornell_box{};
    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto scene = make_light_scene(resources, floor.positions, floor.materials, cam);
    REQUIRE(scene.has_value());
    scene.value().max_bounces = 2; // the second segment is the bounce ray, and it escapes
    scene.value().samples_per_pixel = 256;

    // Wide, so the bounce ray escapes into it often enough to carry a real share of the weight.
    auto const down = tg::vec3f(0, -1, 0);
    auto const lights = cc::vector<sv::light>{
        sv::light::directional(down).lux(2.0f),
        sv::light::sun(down, 60_deg_f).lux(2.0f),
    };

    auto tables = cc::vector<sv::pt_light_table>();
    for (auto const& l : lights)
        tables.push_back(table_of(cc::span<sv::light const>(&l, 1)));

    auto const traced = trace_under(&ctx, &resources, scene.value(), tables);
    auto const& images = co_await traced;
    REQUIRE(images.size() == tables.size());

    auto const size = scene.value().size;
    auto const reference = mean_brightness(images[0], size, 2);
    CHECK(reference > 0.1f);
    CHECK(tg::abs(mean_brightness(images[1], size, 2) - reference) < 0.03f * reference);

    co_await cc::async_settled(sv::background_work(ctx));
}

// One nit is one unit of the tracer's radiance, and that has to hold on both sides of it: for a surface's own emission
// (OpenPBR's `emission_luminance`) and for an area light's `nits`.
// Otherwise an emissive mesh and a light of the same luminance would disagree, by a constant nobody could name.
//
// The surface side is read directly: a camera looking at an emitter of L sees L.
// The light side is read through a floor, since a light is not seen by the camera: a rect of L nits wide enough to fill the
// sky lights the floor exactly as a uniform sky of radiance L does, and the sky is the scale the camera reads.
ASYNC_INVOCABLE_TEST("sv - a surface's emission and an area light's nits are the same radiance",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const luminance = 0.7f;
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;

    // A camera looking at an emitter of L sees L.
    {
        // Black and emissive, so the pixel is the emission and nothing it reflects.
        auto panel = sv_test::cornell_box{};
        auto const emitter = sv_test::pbr_material{.base_color = tg::vec3f(0, 0, 0),
                                                   .emissive = tg::vec3f(luminance, luminance, luminance)};
        sv_test::cb_push_quad(panel, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                              emitter);

        auto scene = make_light_scene(resources, panel.positions, panel.materials, cam);
        REQUIRE(scene.has_value());

        auto const traced = trace_under(&ctx, &resources, scene.value(), {table_of({})});
        auto const& images = co_await traced;
        REQUIRE(images.size() == 1);
        CHECK(tg::abs(mean_brightness(images[0], scene.value().size, 2) - luminance) < 0.01f * luminance);
    }

    // A rect of L nits filling the sky lights a floor as a sky of radiance L does.
    {
        auto floor = sv_test::cornell_box{};
        auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
        sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                              white);

        auto scene = make_light_scene(resources, floor.positions, floor.materials, cam);
        REQUIRE(scene.has_value());
        scene.value().max_bounces = 2; // the second ray is the BSDF half of the rect's weighting, and it escapes
        scene.value().samples_per_pixel = 256;

        // Half a unit up and forty wide, so it covers all but the last degree and a half above the horizon — under a
        // thousandth of what a cosine-weighted floor receives.
        auto const ceiling
            = sv::light::rect(tg::pos3f(0, 0.5f, 0), tg::vec3f(20, 0, 0), tg::vec3f(0, 0, 20)).nits(luminance);
        auto const lit_by_light
            = trace_under(&ctx, &resources, scene.value(), {table_of(cc::span<sv::light const>(&ceiling, 1))});
        auto const& by_light = co_await lit_by_light;
        REQUIRE(by_light.size() == 1);

        auto sky_scene = scene.value();
        sky_scene.environment = sv::background::uniform(tg::vec3f(luminance, luminance, luminance));
        auto const lit_by_sky = trace_under(&ctx, &resources, sky_scene, {table_of({})});
        auto const& by_sky = co_await lit_by_sky;
        REQUIRE(by_sky.size() == 1);

        auto const under_light = mean_brightness(by_light[0], sky_scene.size, 2);
        auto const under_sky = mean_brightness(by_sky[0], sky_scene.size, 2);
        CHECK(under_sky > 0.1f);
        CHECK(tg::abs(under_light - under_sky) < 0.03f * under_sky);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// A light that casts no shadow lights a covered floor exactly as it lights an uncovered one.
//
// Next-event estimation skips the shadow ray for it, and the bounce ray credits it past the surface in the way — both
// halves, or a sun's two strategies stop agreeing on what they integrate and the covered floor comes out darker.
// The cover sits off to the side, across the path toward the light and out of the camera's line to the floor.
ASYNC_INVOCABLE_TEST("sv - a light that casts no shadow ignores what stands in its way",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
    auto open = sv_test::cornell_box{};
    sv_test::cb_push_quad(open, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);
    auto covered = open;
    // Low and wide, so it blocks the whole of a wide sun's cone, and stopping just short of x = 0, where the camera's line
    // to the floor's center passes.
    sv_test::cb_push_quad(covered, tg::pos3f(-3, 0.5f, -2), tg::pos3f(-3, 0.5f, 2), tg::pos3f(-0.1f, 0.5f, 2),
                          tg::pos3f(-0.1f, 0.5f, -2), white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto open_scene = make_light_scene(resources, open.positions, open.materials, cam);
    auto covered_scene = make_light_scene(resources, covered.positions, covered.materials, cam);
    REQUIRE(open_scene.has_value());
    REQUIRE(covered_scene.has_value());
    open_scene.value().max_bounces = 2;
    covered_scene.value().max_bounces = 2;
    // The comparison reads a 4 by 4 window under a wide sun, which 64 samples leave noisy to a few percent.
    open_scene.value().samples_per_pixel = 512;
    covered_scene.value().samples_per_pixel = 512;

    // Travelling down and toward +x, so the path from the floor's center back to the light passes under the cover.
    // The sun is wide on purpose: a narrow one leaves next-event estimation nearly all the weight, and a bounce ray that
    // failed to credit it past the cover would then hide inside the tolerance.
    auto const travel = tg::vec3f(1, -1, 0);
    for (auto const& shadowing : {sv::light::directional(travel).lux(2), sv::light::sun(travel, 60_deg_f).lux(2)})
    {
        auto unshadowed = shadowing;
        unshadowed.casts_shadows(false);

        auto const covered_traces = trace_under(
            &ctx, &resources, covered_scene.value(),
            {table_of(cc::span<sv::light const>(&shadowing, 1)), table_of(cc::span<sv::light const>(&unshadowed, 1))});
        auto const& covered_images = co_await covered_traces;
        auto const open_traces
            = trace_under(&ctx, &resources, open_scene.value(), {table_of(cc::span<sv::light const>(&shadowing, 1))});
        auto const& open_images = co_await open_traces;
        REQUIRE(covered_images.size() == 2);
        REQUIRE(open_images.size() == 1);

        auto const size = open_scene.value().size;
        auto const shadowed = mean_brightness(covered_images[0], size, 2);
        auto const ignored = mean_brightness(covered_images[1], size, 2);
        auto const clear = mean_brightness(open_images[0], size, 2);

        // The cover really is in the way — or the comparison below proves nothing.
        CHECK(shadowed < 0.5f * clear);
        CHECK(tg::abs(ignored - clear) < 0.03f * clear).dump("ignored", ignored).dump("clear", clear);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// A surface reflects only light on its face's own side, however far its authored normal leans off the face.
//
// The light here comes from below the floor and casts no shadow, so nothing but that rule keeps it off the side the camera sees.
// The same light mirrored above the floor shows the setup lights at all.
ASYNC_INVOCABLE_TEST("sv - a surface whose authored normal leans off its face is not lit from behind",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 0.5f};
    auto ground = sv_test::cornell_box{};
    sv_test::cb_push_quad(ground, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);
    auto mesh = sv_test::as_mesh("tilted floor", ground.positions, ground.materials);

    // The authored normal leans 55 degrees off the floor's +y toward -x: it faces both lights and the camera.
    // The frame is +z turned a quarter turn about cross(+z, authored), which carries +z onto it.
    auto const authored = tg::normalize(tg::vec3f(-1.0f, 0.7f, 0.0f));
    auto const half = tg::sqrt(0.5f);
    auto const frame = tg::vec4f(-authored[1] * half, authored[0] * half, 0.0f, half);
    mesh.attributes.push_back(
        sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex,
                                   cc::vector<tg::vec4f>::create_filled(ground.positions.size(), frame)));

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto scene = make_light_scene(resources, mesh, cam);
    REQUIRE(scene.has_value());
    // The second segment is the continuation, which reaches the rect below the floor directly.
    scene.value().max_bounces = 2;

    // A parallel light reaches the floor through next-event alone, and a rect off toward -x through the continuation too.
    struct mirrored
    {
        sv::light above;
        sv::light below;
    };
    auto const pairs = cc::vector<mirrored>{
        {.above = sv::light::directional(tg::vec3f(1, -1, 0)).lux(2).casts_shadows(false),
         .below = sv::light::directional(tg::vec3f(1, 1, 0)).lux(2).casts_shadows(false)},
        {.above
         = sv::light::rect(tg::pos3f(-3, 1, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 2)).nits(4).casts_shadows(false),
         .below
         = sv::light::rect(tg::pos3f(-3, -1, 0), tg::vec3f(0, 0, 2), tg::vec3f(1, 0, 0)).nits(4).casts_shadows(false)},
    };
    for (auto const& pair : pairs)
    {
        auto const traced = trace_under(
            &ctx, &resources, scene.value(),
            {table_of(cc::span<sv::light const>(&pair.above, 1)), table_of(cc::span<sv::light const>(&pair.below, 1))});
        auto const& images = co_await traced;
        REQUIRE(images.size() == 2);

        auto const size = scene.value().size;
        auto const lit = mean_brightness(images[0], size, 2);
        CHECK(lit > 0.1f);
        CHECK(mean_brightness(images[1], size, 2) < 0.01f * lit);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// Which side a surface's authored normal is turned to is the face's to decide, never the camera's.
//
// The floor's authored normal leans toward -x, and the light is straight above.
// From +x the camera is behind that normal yet in front of the face: the floor stays lit, only seen from elsewhere.
ASYNC_INVOCABLE_TEST("sv - a surface lit along its authored normal stays lit from a camera behind that normal",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
    auto ground = sv_test::cornell_box{};
    sv_test::cb_push_quad(ground, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);
    auto mesh = sv_test::as_mesh("leaning floor", ground.positions, ground.materials);

    // A quarter turn about cross(+z, authored) carries +z onto the authored normal, 59 degrees off the floor's +y.
    auto const authored = tg::normalize(tg::vec3f(-1.0f, 0.6f, 0.0f));
    auto const half = tg::sqrt(0.5f);
    auto const frame = tg::vec4f(-authored[1] * half, authored[0] * half, 0.0f, half);
    mesh.attributes.push_back(
        sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex,
                                   cc::vector<tg::vec4f>::create_filled(ground.positions.size(), frame)));

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto from_front = sv::camera::looking_at(tg::pos3d(-3, 3, 0), tg::pos3d(0, 0, 0));
    from_front.projection.aspect_ratio = 1.0;
    auto from_behind = sv::camera::looking_at(tg::pos3d(4, 2, 0), tg::pos3d(0, 0, 0));
    from_behind.projection.aspect_ratio = 1.0;

    auto front_scene = make_light_scene(resources, mesh, from_front);
    REQUIRE(front_scene.has_value());
    front_scene.value().max_bounces = 1;
    auto behind_scene = front_scene.value();
    behind_scene.camera = from_behind;

    auto const overhead = sv::light::directional(tg::vec3f(0, -1, 0)).lux(2);
    auto const front_traced
        = trace_under(&ctx, &resources, front_scene.value(), {table_of(cc::span<sv::light const>(&overhead, 1))});
    auto const& front_images = co_await front_traced;
    auto const behind_traced
        = trace_under(&ctx, &resources, behind_scene, {table_of(cc::span<sv::light const>(&overhead, 1))});
    auto const& behind_images = co_await behind_traced;
    REQUIRE(front_images.size() == 1);
    REQUIRE(behind_images.size() == 1);

    auto const size = behind_scene.size;
    auto const seen_from_front = mean_brightness(front_images[0], size, 2);
    CHECK(seen_from_front > 0.1f);
    CHECK(mean_brightness(behind_images[0], size, 2) > 0.5f * seen_from_front);

    co_await cc::async_settled(sv::background_work(ctx));
}

// An opaque surface has no interior, so its back face reflects exactly as its front face does.
//
// The camera looks down and the light comes in low, more than 85 degrees from the view over every pixel measured.
// That puts each half vector past a dielectric's critical angle.
// Read from inside, that angle reflects the whole lobe; read from outside, as an opaque surface always must be, it is only Schlick's share.
ASYNC_INVOCABLE_TEST("sv - an opaque surface reflects the same from its back face as from its front",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 0.74f};
    auto facing_up = sv_test::cornell_box{};
    sv_test::cb_push_quad(facing_up, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);
    auto facing_down = sv_test::cornell_box{};
    sv_test::cb_push_quad(facing_down, tg::pos3f(-5, 0, -5), tg::pos3f(5, 0, -5), tg::pos3f(5, 0, 5),
                          tg::pos3f(-5, 0, 5), white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto up_scene = make_light_scene(resources, facing_up.positions, facing_up.materials, cam);
    auto down_scene = make_light_scene(resources, facing_down.positions, facing_down.materials, cam);
    REQUIRE(up_scene.has_value());
    REQUIRE(down_scene.has_value());
    up_scene.value().max_bounces = 1;
    down_scene.value().max_bounces = 1;

    auto const low = sv::light::directional(tg::vec3f(-1.0f, -0.04f, 0.0f)).lux(80);
    auto const up_traced
        = trace_under(&ctx, &resources, up_scene.value(), {table_of(cc::span<sv::light const>(&low, 1))});
    auto const& up_images = co_await up_traced;
    auto const down_traced
        = trace_under(&ctx, &resources, down_scene.value(), {table_of(cc::span<sv::light const>(&low, 1))});
    auto const& down_images = co_await down_traced;
    REQUIRE(up_images.size() == 1);
    REQUIRE(down_images.size() == 1);

    auto const size = up_scene.value().size;
    auto const front = mean_brightness(up_images[0], size, 2);
    CHECK(front > 0.1f);
    CHECK(tg::abs(mean_brightness(down_images[0], size, 2) - front) < 0.03f * front);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A light the camera may see is seen at its own radiance, and one it may not is not seen at all.
// The rect faces the camera and away from the floor, so the floor under it stays dark either way.
ASYNC_INVOCABLE_TEST("sv - a light visible to the camera is seen at its radiance", (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto floor = sv_test::cornell_box{};
    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto const scene = make_light_scene(resources, floor.positions, floor.materials, cam);
    REQUIRE(scene.has_value());

    // cross(+z, +x) is +y: the rect faces up, toward the camera.
    auto const luminance = 3.0f;
    auto hidden = sv::light::rect(tg::pos3f(0, 2, 0), tg::vec3f(0, 0, 0.5f), tg::vec3f(0.5f, 0, 0)).nits(luminance);
    auto seen = hidden;
    seen.visible_to_camera();

    auto const traced
        = trace_under(&ctx, &resources, scene.value(),
                      {table_of(cc::span<sv::light const>(&hidden, 1)), table_of(cc::span<sv::light const>(&seen, 1))});
    auto const& images = co_await traced;
    REQUIRE(images.size() == 2);

    auto const size = scene.value().size;
    CHECK(mean_brightness(images[0], size, 2) < 0.01f);
    CHECK(tg::abs(mean_brightness(images[1], size, 2) - luminance) < 0.01f * luminance);

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// `a` x `b` as a vector, which a tangent frame is built from.
[[nodiscard]] tg::vec3f cross_of(tg::vec3f a, tg::vec3f b)
{
    return tg::vec3f(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
}

/// A tangent frame as the `tangent_frame` attribute carries it: tangent, bitangent and normal as the quaternion's x, y and z.
[[nodiscard]] tg::quat_f frame_quat(tg::vec3f t, tg::vec3f n)
{
    auto const b = cross_of(n, t);
    return tg::quat_f::make_from_basis(t, b, n).normalized();
}

/// The floor, a matte gray, with `frame` and `handedness` at every corner and uvs across it.
[[nodiscard]] sv::mesh framed_floor(tg::quat_f frame, f32 handedness)
{
    auto const gray = sv_test::pbr_material{.base_color = tg::vec3f(0.5f, 0.5f, 0.5f), .roughness = 1.0f};
    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          gray);
    auto mesh = sv_test::as_mesh("normal-mapped floor", floor.positions, floor.materials);

    auto const count = floor.positions.size();
    auto uvs = cc::vector<tg::vec2f>();
    for (auto const& p : floor.positions)
        uvs.push_back(tg::vec2f(p[0], p[2]));
    mesh.attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, cc::move(uvs)));
    mesh.attributes.push_back(sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex,
                                                         cc::vector<tg::quat_f>::create_filled(count, frame)));
    mesh.attributes.push_back(sv::mesh_attribute::create("tangent_handedness", sv::attribute_frequency::per_vertex,
                                                         cc::vector<f32>::create_filled(count, handedness)));
    return mesh;
}
} // namespace

// A normal map against its own definition: a tangent-space normal read from a texture means the shading normal
// `t * x + b * y + n * z` of the corner's frame, b mirrored where the handedness is negative.
// So a flat floor with a constant normal map must shade as the same floor does whose frame is turned onto that normal and
// which samples no map.
// Four tilts, one toward each tangent direction, under a light that tells all four apart, catch a swapped or a negated
// axis and a wrong strength; a frame turned a quarter about its normal, a mirrored one, and both encodings a normal map
// arrives in, signed or unsigned with its transform, catch what only those reach.
ASYNC_INVOCABLE_TEST("sv - a normal map shades as the tangent frame turned onto its normal does",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    struct frame_case
    {
        cc::string_view name;
        tg::vec3f tangent;
        f32 handedness;
    };
    auto const up = tg::vec3f(0, 1, 0);
    auto const frames = cc::vector<frame_case>{
        {.name = "tangent along +x", .tangent = tg::vec3f(1, 0, 0), .handedness = 1.0f},
        {.name = "tangent along -z, a quarter turn", .tangent = tg::vec3f(0, 0, -1), .handedness = 1.0f},
        {.name = "mirrored", .tangent = tg::vec3f(1, 0, 0), .handedness = -1.0f},
    };

    // From above, off every axis, so a tilt toward each of the four tangent directions is lit differently.
    auto const light = sv::light::directional(tg::normalize(tg::vec3f(0.7f, -1.0f, 0.35f))).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;

    auto const tilt = tg::angle_f::make_from_degree(40.0f);
    for (auto const& f : frames)
        for (auto const is_signed : {true, false})
        {
            auto shades = cc::vector<f32>();
            for (auto const azimuth_degrees : {0.0f, 90.0f, 180.0f, 270.0f})
            {
                auto const azimuth = tg::angle_f::make_from_degree(azimuth_degrees);
                auto const wanted
                    = tg::vec3f(tg::sin(tilt) * tg::cos(azimuth), tg::sin(tilt) * tg::sin(azimuth), tg::cos(tilt));

                // Encoded as a normal map stores it, and decoded as the sampler returns it, so the reference has the
                // quantized normal rather than the exact one.
                auto pixels = cc::vector<byte>();
                auto decoded = tg::vec3f();
                for (auto c = 0; c < 3; ++c)
                {
                    if (is_signed)
                    {
                        auto const q = i32(tg::round(wanted[c] * 127.0f));
                        pixels.push_back(byte(u8(i8(q))));
                        decoded[c] = f32(q) / 127.0f;
                    }
                    else
                    {
                        auto const q = i32(tg::round((wanted[c] * 0.5f + 0.5f) * 255.0f));
                        pixels.push_back(byte(u8(q)));
                        decoded[c] = f32(q) / 255.0f * 2.0f - 1.0f;
                    }
                }
                pixels.push_back(byte(is_signed ? 127 : 255));
                auto texels = cc::vector<byte>();
                for (auto i = 0; i < 4 * 4; ++i)
                    texels.push_back_range(pixels);
                decoded = tg::normalize(decoded);

                auto mapped = framed_floor(frame_quat(f.tangent, up), f.handedness);
                mapped.textures.push_back(
                    {.name = "normal",
                     .source
                     = {.texture = sv::texture_data::create(
                            cc::move(texels), is_signed ? sg::pixel_format::rgba8_snorm : sg::pixel_format::rgba8_unorm,
                            4, 4),
                        .transform = is_signed ? sv::sample_transform{} : sv::sample_transform::of_signed_normal()}});

                // The same normal in world space, and a frame turned onto it, with no map to sample.
                auto const bitangent = cross_of(up, f.tangent) * f.handedness;
                auto const world = tg::normalize(f.tangent * decoded[0] + bitangent * decoded[1] + up * decoded[2]);
                auto const level = tg::normalize(f.tangent - world * tg::dot(f.tangent, world));
                auto const reference = framed_floor(frame_quat(level, world), 1.0f);

                auto shade = cc::fixed_array<f32, 2>{};
                auto const meshes = cc::fixed_array<sv::mesh const*, 2>{&mapped, &reference};
                for (auto k = 0; k < 2; ++k)
                {
                    auto scene = make_light_scene(resources, *meshes[k], cam);
                    REQUIRE(scene.has_value());
                    auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
                    auto const& images = co_await traced;
                    REQUIRE(images.size() == 1);
                    shade[k] = mean_brightness(images[0], scene.value().size);
                }
                auto const shade_mapped = shade[0];
                auto const shade_reference = shade[1];
                CHECK(tg::abs(shade_mapped - shade_reference) <= 0.02f * shade_reference)
                    .dump("frame", f.name)
                    .dump("signed", is_signed)
                    .dump("azimuth", azimuth_degrees)
                    .dump("mapped", shade_mapped)
                    .dump("reference", shade_reference);
                shades.push_back(shade_reference);
            }

            // The light tells the four tilts apart, or the comparison above could not catch a swapped axis.
            auto lo = shades[0];
            auto hi = shades[0];
            for (auto const s : shades)
            {
                lo = cc::min(lo, s);
                hi = cc::max(hi, s);
            }
            CHECK(hi - lo > 0.2f * hi).dump("frame", f.name).dump("darkest", lo).dump("brightest", hi);
            for (auto i = isize(0); i < shades.size(); ++i)
                for (auto j = i + 1; j < shades.size(); ++j)
                    CHECK(tg::abs(shades[i] - shades[j]) > 0.03f * hi).dump("frame", f.name).dump("i", i).dump("j", j);
        }

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// A floor `k` uv units to the meter, sampling a 64 by 64 base color whose level L holds 2^-L, or `flat` at every level.
[[nodiscard]] sv::mesh mipped_floor(f32 k, bool halving, f32 flat = 1.0f)
{
    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-50, 0, -50), tg::pos3f(-50, 0, 50), tg::pos3f(50, 0, 50),
                          tg::pos3f(50, 0, -50), sv_test::pbr_material{});
    auto uvs = cc::vector<tg::vec2f>();
    for (auto const& p : floor.positions)
        uvs.push_back(tg::vec2f(p[0] * k, p[2] * k));
    auto attributes = cc::vector<sv::mesh_attribute>();
    attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, cc::move(uvs)));
    auto mesh = sv::mesh{.name = "mipped floor",
                         .geometry = sv::triangle_geometry::create_from_positions(floor.positions),
                         .attributes = cc::move(attributes),
                         .material = sv::default_material(sv_test::shared_material_library())};

    auto pixels = cc::vector<byte>();
    auto const levels = sv::impl::mip_count_of(64, 64);
    for (auto level = 0; level < levels; ++level)
    {
        auto const value = byte(u8(tg::round(255.0f * (halving ? 1.0f / f32(1 << level) : flat))));
        auto const e = sv::impl::mip_extent(64, level);
        for (auto i = 0; i < e * e; ++i)
            pixels.push_back_range(cc::fixed_array<byte, 4>{value, value, value, byte(255)});
    }
    auto const repeat
        = sg::sampler{.address_u = sg::sampler_address_mode::repeat, .address_v = sg::sampler_address_mode::repeat};
    mesh.textures.push_back(
        {.name = "base_color",
         .source = {.texture = sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 64, 64, levels),
                    .sampler = repeat}});
    return mesh;
}

/// The level a sample of a chain whose level L holds 2^-L reads, from what it read: linear between two levels, 2^-n (1 - f / 2) at n + f.
[[nodiscard]] f32 level_of(f32 ratio)
{
    auto const n = tg::floor(-(tg::log(ratio) / tg::log(2.0f)));
    auto const f = 2.0f * (1.0f - ratio * tg::pow(2.0f, n));
    return n + f;
}
} // namespace

// A ray cone picks a texture's level from its footprint: the pixel's angle times the distance, over the cosine it meets
// the face at, against the texels the triangle spans per meter (Akenine-Moller et al. 2019).
// A floor whose levels each hold half the one above shows the level a pixel read, against the same floor at 1 throughout,
// so the light cancels: the camera's level is the one the cone predicts, and four times as far is two levels coarser.
ASYNC_INVOCABLE_TEST("sv - a ray cone samples the texture level its footprint covers", (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const light = sv::light::directional(tg::vec3f(0, -1, 0)).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));
    auto resources = sv::gpu_resource_manager::create(ctx);

    // 2 uv units a meter, of 64 texels each: 128 texels a meter
    auto const k = 2.0f;
    auto measured = cc::vector<f32>();
    for (auto const height : {3.0, 12.0})
    {
        auto cam = sv::camera::looking_at(tg::pos3d(0, height, -0.01), tg::pos3d(0, 0, 0));
        cam.projection.aspect_ratio = 1.0;

        // halving, white and black at every level: what the albedo does not scale, the specular, falls out of the ratio
        auto shade = cc::fixed_array<f32, 3>{};
        for (auto i = 0; i < 3; ++i)
        {
            auto scene = make_light_scene(resources, mipped_floor(k, i == 0, i == 1 ? 1.0f : 0.0f), cam);
            REQUIRE(scene.has_value());
            auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
            auto const& images = co_await traced;
            REQUIRE(images.size() == 1);
            shade[i] = mean_brightness(images[0], scene.value().size, 2);
        }
        REQUIRE(shade[1] > shade[2]);
        auto const level = level_of((shade[0] - shade[2]) / (shade[1] - shade[2]));

        // the footprint at the image's center, straight down: the pixel's angle times the height, at 128 texels a meter
        auto const record = sv::camera_record_of(cam);
        auto const spread = 2.0f * record.up_scaled.length() / 48.0f;
        auto const wanted = tg::log(spread * f32(height) * k * 64.0f) / tg::log(2.0f);
        CHECK(tg::abs(level - wanted) < 0.35f).dump("height", height).dump("level", level).dump("wanted", wanted);
        measured.push_back(level);
    }
    CHECK(tg::abs(measured[1] - measured[0] - 2.0f) < 0.2f).dump("near", measured[0]).dump("far", measured[1]);

    co_await cc::async_settled(sv::background_work(ctx));
}

// An orthographic camera's rays run parallel, so its cone starts a pixel wide and never widens.
// The halving floor seen straight down through it reads one level at 3 m above it as at 12: the level its pixel's width predicts,
// where a pinhole's reads two levels coarser four times as far.
ASYNC_INVOCABLE_TEST("sv - an orthographic ray cone samples the level of its pixel's width, at any distance",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const light = sv::light::directional(tg::vec3f(0, -1, 0)).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));
    auto resources = sv::gpu_resource_manager::create(ctx);

    // 2 uv units a meter, of 64 texels each: 128 texels a meter; the image 4 m high over 48 pixels
    auto const k = 2.0f;
    auto const view_height = 4.0;
    auto measured = cc::vector<f32>();
    for (auto const height : {3.0, 12.0})
    {
        auto cam = sv::camera::looking_at(tg::pos3d(0, height, -0.01), tg::pos3d(0, 0, 0));
        cam.projection.aspect_ratio = 1.0;
        cam.projection.kind = sv::projection_kind::orthographic;
        cam.projection.view_height = view_height;

        auto shade = cc::fixed_array<f32, 3>{};
        for (auto i = 0; i < 3; ++i)
        {
            auto scene = make_light_scene(resources, mipped_floor(k, i == 0, i == 1 ? 1.0f : 0.0f), cam);
            REQUIRE(scene.has_value());
            auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
            auto const& images = co_await traced;
            REQUIRE(images.size() == 1);
            shade[i] = mean_brightness(images[0], scene.value().size, 2);
        }
        REQUIRE(shade[1] > shade[2]);
        measured.push_back(level_of((shade[0] - shade[2]) / (shade[1] - shade[2])));
    }

    auto const wanted = tg::log(f32(view_height) / 48.0f * k * 64.0f) / tg::log(2.0f);
    CHECK(tg::abs(measured[0] - wanted) < 0.35f).dump("near", measured[0]).dump("wanted", wanted);
    CHECK(tg::abs(measured[1] - wanted) < 0.35f).dump("far", measured[1]).dump("wanted", wanted);
    CHECK(tg::abs(measured[1] - measured[0]) < 0.1f).dump("near", measured[0]).dump("far", measured[1]);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A grazing view stretches a cone's footprint along the floor by one over the cosine, and an anisotropic sampler reads
// that ellipse at the level of its short axis, the cone's own width, with more taps along the long one.
// The same halving floor seen 70 degrees off its normal reads the level the width predicts, where an isotropic footprint
// would read log2(1 / cos 70), about 1.55 levels, coarser.
ASYNC_INVOCABLE_TEST("sv - a grazing ray cone samples at its footprint's short axis, filtering anisotropically",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const light = sv::light::directional(tg::vec3f(0, -1, 0)).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));
    auto resources = sv::gpu_resource_manager::create(ctx);

    // 3 m from the floor's center, 70 degrees off its normal
    auto const k = 2.0f;
    auto const distance = 3.0;
    auto const tilt = tg::angle_d::make_from_degree(70.0);
    auto cam
        = sv::camera::looking_at(tg::pos3d(0, distance * tg::cos(tilt), -distance * tg::sin(tilt)), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;

    auto shade = cc::fixed_array<f32, 3>{};
    for (auto i = 0; i < 3; ++i)
    {
        auto scene = make_light_scene(resources, mipped_floor(k, i == 0, i == 1 ? 1.0f : 0.0f), cam);
        REQUIRE(scene.has_value());
        auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
        auto const& images = co_await traced;
        REQUIRE(images.size() == 1);
        shade[i] = mean_brightness(images[0], scene.value().size, 2);
    }
    REQUIRE(shade[1] > shade[2]);
    auto const level = level_of((shade[0] - shade[2]) / (shade[1] - shade[2]));

    // the short axis at the image's center: the pixel's angle times the distance, at 128 texels a meter
    // a GPU's anisotropic level selection is an approximation of its own, so the bound is wider than the isotropic test's
    auto const record = sv::camera_record_of(cam);
    auto const spread = 2.0f * record.up_scaled.length() / 48.0f;
    auto const wanted = tg::log(spread * f32(distance) * k * 64.0f) / tg::log(2.0f);
    CHECK(tg::abs(level - wanted) < 0.5f).dump("level", level).dump("wanted", wanted);

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// A floor of `type`, a diffuse gray with no specular, its occlusion `ao`.
[[nodiscard]] sv::mesh occluded_floor(cc::string_view type, f32 ao)
{
    auto& lib = sv_test::shared_material_library();
    auto const material
        = lib.acquire(sv::material::create(cc::format("{} floor at {}", type, ao), lib.acquire_type(type).value(),
                                           {sv::material_attribute_binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f)),
                                            sv::material_attribute_binding::of("specular_weight", 0.0f),
                                            sv::material_attribute_binding::of("occlusion", ao)}));
    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          sv_test::pbr_material{});
    return {.name = "occluded floor",
            .geometry = sv::triangle_geometry::create_from_positions(floor.positions),
            .material = material};
}
} // namespace

// A tileable material's occlusion is its own relief's, which no trace can find, so the relief type applies it itself:
// light from direction l reaches a texel by clamp(|n.l| + 2 ao^2 - 1, 0, 1), the micro-shadowing of Chan 2018.
// A floor lit only by a light 60 degrees off its normal shows exactly that against the same floor fully open,
// and at an occlusion of 1 the relief type is the plain OpenPBR one, which ignores occlusion as an imported asset needs.
ASYNC_INVOCABLE_TEST("sv - a relief material's occlusion closes grazing light, by the micro-shadowing of its ao",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    // 60 degrees off the floor's normal, so |n.l| is a half
    auto const light
        = sv::light::directional(tg::normalize(tg::vec3f(tg::sqrt(3.0f), -1.0f, 0.0f))).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;

    auto const shade = [&](cc::string_view type, f32 ao) -> cc::shared_async<f32>
    {
        auto scene = make_light_scene(resources, occluded_floor(type, ao), cam);
        if (!scene.has_value())
            co_return -1.0f;
        auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
        auto const& images = co_await traced;
        co_return images.size() == 1 ? mean_brightness(images[0], scene.value().size, 4) : -1.0f;
    };

    auto values = cc::vector<f32>();
    for (auto const& [type, ao] : {cc::pair<cc::string_view, f32>{sv::builtin_material::openpbr, 0.6f},
                                   {sv::builtin_material::openpbr_relief, 1.0f},
                                   {sv::builtin_material::openpbr_relief, 0.8f},
                                   {sv::builtin_material::openpbr_relief, 0.6f}})
    {
        auto const traced = shade(type, ao);
        auto const v = co_await traced;
        REQUIRE(v > 0.0f);
        values.push_back(v);
    }

    // the plain type ignores occlusion, and the relief type fully open is the same surface
    CHECK(tg::abs(values[1] - values[0]) < 0.01f * values[0]).dump("openpbr", values[0]).dump("relief", values[1]);
    // 0.5 + 2 * 0.64 - 1 and 0.5 + 2 * 0.36 - 1
    CHECK(tg::abs(values[2] / values[1] - 0.78f) < 0.02f).dump("ratio", values[2] / values[1]);
    CHECK(tg::abs(values[3] / values[1] - 0.22f) < 0.02f).dump("ratio", values[3] / values[1]);

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// A floor at height `y`, uv `k` per meter of x and z, striped in u, of the relief type: its height texture holds
/// `height` everywhere in the band from `top` down to `bottom`.
[[nodiscard]] sv::mesh relief_floor(f32 y, f32 k, f32 height, f32 top, f32 bottom)
{
    auto& lib = sv_test::shared_material_library();
    auto const material
        = lib.acquire(sv::material::create(cc::format("relief floor {} {} {}", height, top, bottom),
                                           lib.acquire_type(sv::builtin_material::openpbr_relief).value(),
                                           {sv::material_attribute_binding::of("specular_weight", 0.0f),
                                            sv::material_attribute_binding::of("height_top", top),
                                            sv::material_attribute_binding::of("height_bottom", bottom)}));
    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-8, y, -8), tg::pos3f(-8, y, 8), tg::pos3f(8, y, 8), tg::pos3f(8, y, -8),
                          sv_test::pbr_material{});
    auto uvs = cc::vector<tg::vec2f>();
    for (auto const& p : floor.positions)
        uvs.push_back(tg::vec2f(p[0] * k, p[2] * k));
    auto attributes = cc::vector<sv::mesh_attribute>();
    attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, cc::move(uvs)));
    auto mesh = sv::mesh{.name = "relief floor",
                         .geometry = sv::triangle_geometry::create_from_positions(floor.positions),
                         .attributes = cc::move(attributes),
                         .material = material};

    auto const repeat
        = sg::sampler{.address_u = sg::sampler_address_mode::repeat, .address_v = sg::sampler_address_mode::repeat};
    // eight stripes across a repeat, black and white
    auto stripes = cc::vector<byte>();
    for (auto row = 0; row < 8; ++row)
        for (auto col = 0; col < 8; ++col)
        {
            auto const v = col % 2 == 0 ? byte(230) : byte(25);
            stripes.push_back_range(cc::fixed_array<byte, 4>{v, v, v, byte(255)});
        }
    mesh.textures.push_back(
        {.name = "base_color",
         .source = {.texture = sv::texture_data::create(cc::move(stripes), sg::pixel_format::rgba8_unorm, 8, 8),
                    .sampler = repeat}});
    auto heights = cc::vector<f32>::create_filled(4 * 4, height);
    mesh.textures.push_back({.name = "height",
                             .source = {.texture = sv::texture_data::create(cc::span<f32 const>(heights).as_bytes(),
                                                                            sg::pixel_format::r32_float, 4, 4),
                                        .sampler = repeat}});
    return mesh;
}

/// The root mean square of two images' difference in brightness.
[[nodiscard]] f32 rms_between(cc::vector<tg::vec4f> const& a, cc::vector<tg::vec4f> const& b)
{
    auto sum = 0.0;
    for (auto i = isize(0); i < a.size(); ++i)
    {
        auto const d = (a[i][0] + a[i][1] + a[i][2] - b[i][0] - b[i][1] - b[i][2]) / 3.0f;
        sum += f64(d) * f64(d);
    }
    return f32(tg::sqrt(sum / f64(a.size())));
}
} // namespace

namespace
{
/// A relief floor whose horizon is `lo` and `hi` everywhere, its tangent along +x and its bitangent along -z, so a
/// horizon's direction 0 faces +x; a diffuse gray with no specular.
[[nodiscard]] sv::mesh horizon_floor(tg::vec4f lo, tg::vec4f hi)
{
    auto& lib = sv_test::shared_material_library();
    auto const material = lib.acquire(sv::material::create(
        cc::format("horizon floor {} {} {} {}", lo[0], lo[1], hi[0], hi[1]),
        lib.acquire_type(sv::builtin_material::openpbr_relief).value(),
        {sv::material_attribute_binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f)),
         sv::material_attribute_binding::of("specular_weight", 0.0f),
         sv::material_attribute_binding::of("horizon_lo", lo), sv::material_attribute_binding::of("horizon_hi", hi)}));
    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          sv_test::pbr_material{});
    auto const frame
        = tg::quat_f::make_from_basis(tg::vec3f(1, 0, 0), tg::vec3f(0, 0, -1), tg::vec3f(0, 1, 0)).normalized();
    return {.name = "horizon floor",
            .geometry = sv::triangle_geometry::create_from_positions(floor.positions),
            .attributes
            = {sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex,
                                          cc::vector<tg::quat_f>::create_filled(floor.positions.size(), frame))},
            .material = material};
}
} // namespace

// A relief's horizon shadows it as the relief would itself: light from below the horizon in its direction is closed,
// and light above it, or from a direction whose horizon is open, is not.
// A floor whose horizon rises to 45 degrees toward +x and nowhere else: a light from +x at 30 degrees is shadowed,
// one from +x at 60 degrees and one from -x at 30 degrees are not, each against the same floor wholly open.
ASYNC_INVOCABLE_TEST("sv - a relief's horizon shadows the light below it, in its own direction alone",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto cam = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    cam.projection.aspect_ratio = 1.0;
    auto const blocked = tg::vec4f(tg::sin(tg::angle_f::make_from_degree(45.0f)), 0, 0, 0);
    auto const open = tg::vec4f(0, 0, 0, 0);

    auto const shade = [&](tg::vec4f lo, f32 from_x, f32 elevation_degrees) -> cc::shared_async<f32>
    {
        auto const e = tg::angle_f::make_from_degree(elevation_degrees);
        // the light travels toward the floor from the side `from_x` names
        auto const light
            = sv::light::directional(tg::vec3f(-from_x * tg::cos(e), -tg::sin(e), 0.0f)).lux(2).casts_shadows(false);
        auto const lights = table_of(cc::span<sv::light const>(&light, 1));
        auto scene = make_light_scene(resources, horizon_floor(lo, open), cam);
        if (!scene.has_value())
            co_return -1.0f;
        auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
        auto const& images = co_await traced;
        co_return images.size() == 1 ? mean_brightness(images[0], scene.value().size, 4) : -1.0f;
    };

    for (auto const [from_x, elevation, wanted, what] :
         {cc::tuple<f32, f32, f32, cc::string_view>{1.0f, 30.0f, 0.0f, "from +x below the horizon"},
          {1.0f, 60.0f, 1.0f, "from +x above it"},
          {-1.0f, 30.0f, 1.0f, "from -x, where it is open"}})
    {
        auto const relief_traced = shade(blocked, from_x, elevation);
        auto const relief = co_await relief_traced;
        auto const plain_traced = shade(open, from_x, elevation);
        auto const plain = co_await plain_traced;
        REQUIRE(relief >= 0.0f);
        REQUIRE(plain > 0.0f);
        CHECK(tg::abs(relief / plain - wanted) < 0.02f).dump("light", what).dump("ratio", relief / plain);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// Parallax occlusion mapping against geometry: a relief whose height is one flat plane 0.2 m below its face must look
// exactly as a face 0.2 m lower does, its stripes moved by the ray's own slant (Tatarchuk 2006).
// The same relief with no band to march, top at the bottom, stays where its face is, which is what the comparison is against.
ASYNC_INVOCABLE_TEST("sv - a relief's parallax shows a sunken plane where real geometry would be",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto const light = sv::light::directional(tg::vec3f(0, -1, 0)).lux(2).casts_shadows(false);
    auto const lights = table_of(cc::span<sv::light const>(&light, 1));
    auto resources = sv::gpu_resource_manager::create(ctx);
    // 45 degrees down onto the floor, so a sunken plane shows its stripes moved by its depth
    auto cam = sv::camera::looking_at(tg::pos3d(0.3, 2.5, -2.5), tg::pos3d(0.3, 0, 0));
    cam.projection.aspect_ratio = 1.0;

    auto const k = 1.5f;
    auto const meshes = cc::fixed_array<sv::mesh, 3>{
        relief_floor(0.0f, k, -0.2f, 0.0f, -0.4f), // the relief, a plane 0.2 m down
        relief_floor(-0.2f, k, 0.0f, 0.0f, 0.0f),  // real geometry 0.2 m down, nothing to march
        relief_floor(0.0f, k, -0.2f, 0.0f, 0.0f),  // the relief's face itself, its band empty
    };
    auto images = cc::vector<cc::vector<tg::vec4f>>();
    for (auto const& mesh : meshes)
    {
        auto scene = make_light_scene(resources, mesh, cam);
        REQUIRE(scene.has_value());
        scene.value().size = 64;
        auto const traced = trace_under(&ctx, &resources, scene.value(), {lights});
        auto const& out = co_await traced;
        REQUIRE(out.size() == 1);
        images.push_back(out[0]);
    }

    auto const parallax_error = rms_between(images[0], images[1]);
    auto const unmoved_error = rms_between(images[2], images[1]);
    CHECK(unmoved_error > 0.05f).dump("unmoved", unmoved_error);
    CHECK(parallax_error < 0.15f * unmoved_error).dump("parallax", parallax_error).dump("unmoved", unmoved_error);

    co_await cc::async_settled(sv::background_work(ctx));
}
