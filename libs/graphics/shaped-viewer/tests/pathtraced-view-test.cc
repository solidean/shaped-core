#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Headless end-to-end path trace.
// It builds a simple Cornell box through the managers, integrates one small view with global illumination, and drives it to completion.
// Beyond the flat direct-lit raytraced-view test, this exercises the whole GI path.
// The path-tracing shaders compile through slib, the DXR pipeline + shader table build, the TLAS is built, and the raygen bounces rays with NEE toward the ceiling light.
//
// No pixel readback: this asserts the pipeline runs rather than inspecting the image (same philosophy as the
// raytraced-view test). Reaching the end without an assert/exception means every GPU stage succeeded.
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
    // (kept small so the trace stays cheap on the WARP software device).
    auto fc = sv::pt_frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(cam);
    // the box light is an axis-aligned XZ rect, emitting straight down
    auto const lights = sv_test::light_table_of(box.light);
    lights.describe_in(fc);
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

            auto const frame = ctx.transient.create_buffer_from_pod(fc, sg::buffer_usage::uniform_buffer);

            auto const background
                = ctx.transient.create_buffer_from_pod(sv::background_gpu::from(bg), sg::buffer_usage::uniform_buffer);

            // rgba32_float, which the routine asserts on: the raygen reads the target back to blend into it.
            auto const target = ctx.transient.create_texture_2d(
                {.format = sg::pixel_format::rgba32_float,
                 .width = size[0],
                 .height = size[1],
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

            // One `sv::instance` per TLAS instance: where this item's material parameters live, and its geometry.
            auto const instance_table = ctx.transient.create_buffer_from_data(records, sg::buffer_usage::readonly_buffer);
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

    // Reaching here means the whole GI pipeline ran (BLAS + TLAS build, DXR dispatch) without a device error.
    CHECK(mesh_rec->triangle_count == box.materials.size());
    CHECK(!mesh_rec->is_indexed); // the non-indexed path: the corner indices come from the primitive, not a buffer
    CHECK(records[0].is_indexed == 0u);

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

        auto const frame = ctx.transient.create_buffer_from_pod(
            sv::pt_frame_constants_gpu{.samples_per_pixel = 1, .max_bounces = 1}, sg::buffer_usage::uniform_buffer);

        auto const background = ctx.transient.create_buffer_from_pod(sv::background_gpu::from(sv::background{}),
                                                                     sg::buffer_usage::uniform_buffer);

        auto const target = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba32_float,
             .width = size[0],
             .height = size[1],
             .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

        auto const instance_table = ctx.transient.create_buffer_from_data(records, sg::buffer_usage::readonly_buffer);

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
    co_await cc::async_settled(permutation->shader);
    REQUIRE(permutation->shader->has_error());

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

    co_await cc::async_settled(sv::background_work(ctx));
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
ASYNC_INVOCABLE_TEST("sv - a path-traced textured material builds its sampler group (headless)",
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
    // the box light is an axis-aligned XZ rect, emitting straight down
    auto const lights = sv_test::light_table_of(box.light);
    lights.describe_in(fc);
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

            auto const frame = ctx.transient.create_buffer_from_pod(fc, sg::buffer_usage::uniform_buffer);

            auto const background = ctx.transient.create_buffer_from_pod(sv::background_gpu::from(sv::background{}),
                                                                         sg::buffer_usage::uniform_buffer);

            auto const instance_table = ctx.transient.create_buffer_from_data(records, sg::buffer_usage::readonly_buffer);
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

/// The mesh `positions` / `materials` describe, acquired and ready to trace, or empty when it did not build.
[[nodiscard]] cc::optional<light_scene> make_light_scene(sv::gpu_resource_manager& resources,
                                                         cc::vector<tg::pos3f> const& positions,
                                                         cc::vector<sv::pbr_material> const& materials,
                                                         sv::camera const& camera)
{
    auto scene = light_scene{};
    scene.item = resources.acquire_scene_item(sv_test::as_mesh("light scene", positions, materials));
    resources.wait_for_pending_uploads();

    auto const* const mesh = resources.meshes.get_ptr(scene.item.mesh);
    auto const* const permutation = resources.shaders.find(scene.item.shader_key);
    if (mesh == nullptr || permutation == nullptr)
        return {};

    scene.instances.push_back(sg::tlas_instance{.blas = mesh->blas, .instance_id = 0, .hit_group_offset = 0});
    scene.hit_groups.push_back(permutation);
    scene.camera = camera;
    return scene;
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
            auto fc = sv::pt_frame_constants_gpu{};
            fc.camera = sv::camera_gpu::from(scene.camera);
            lights.describe_in(fc);
            fc.samples_per_pixel = scene.samples_per_pixel;
            fc.max_bounces = scene.max_bounces;
            fc.seed = 1u;

            auto const frame = ctx->transient.create_buffer_from_pod(fc, sg::buffer_usage::uniform_buffer);

            auto const background = ctx->transient.create_buffer_from_pod(sv::background_gpu::from(scene.environment),
                                                                          sg::buffer_usage::uniform_buffer);

            auto const target = ctx->transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                                  .width = scene.size,
                                                                  .height = scene.size,
                                                                  .usage = sg::texture_usage::readonly_texture
                                                                         | sg::texture_usage::readwrite_texture
                                                                         | sg::texture_usage::copy_src});

            auto records = cc::vector<sv::instance_gpu>();
            records.push_back(resources->describe_instance(*cmd, scene.item.mesh, scene.item.instance));
            auto const instance_table
                = ctx->transient.create_buffer_from_data(records, sg::buffer_usage::readonly_buffer);

            // A table with no lights binds nothing, which is the routine's own stand-in path.
            auto const light_buffer
                = lights.records.empty() ? sg::buffer<sv::light_gpu>() : sv_test::upload_lights(*cmd, lights);

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
    auto records = cc::vector<sv::light_gpu>();
    for (auto const& l : lights)
        records.push_back(sv::light_gpu::from(l));
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
        return cc::string_view("no DXC compiler to build the path-tracing shaders");
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    // A white floor at y = 0, seen from above.
    auto floor = sv_test::cornell_box{};
    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    auto floor = sv_test::cornell_box{};
    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
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
        auto const emitter
            = sv::pbr_material{.base_color = tg::vec3f(0, 0, 0), .emissive = tg::vec3f(luminance, luminance, luminance)};
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
        auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    using namespace tg::literals;

    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
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
        CHECK(tg::abs(ignored - clear) < 0.03f * clear);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// A light the camera may see is seen at its own radiance, and one it may not is not seen at all.
// The rect faces the camera and away from the floor, so the floor under it stays dark either way.
ASYNC_INVOCABLE_TEST("sv - a light visible to the camera is seen at its radiance", (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;
    if (auto const reason = cannot_trace(ctx); reason.has_value())
        SKIP(reason.value());

    auto floor = sv_test::cornell_box{};
    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 1.0f};
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
