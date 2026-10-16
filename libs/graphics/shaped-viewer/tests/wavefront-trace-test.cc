#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

// The wavefront tracer against the megakernel it replaces, on the same scene and the same estimator.
//
// Both integrate one path per sample, five bounces deep with no roulette, so their means agree up to Monte Carlo noise.
// The comparison is of block means rather than of pixels, which at a few hundred samples each are still noise.

namespace
{
using namespace cc::primitive_defines;

constexpr auto k_size = tg::vec2i(32, 32);
constexpr int k_block = 4;
/// The Cornell box's path length, for both tracers.
constexpr int k_max_bounces = 5;

/// The scene both tracers render: the Cornell box through the managers, its instances and its light table.
struct cornell_scene
{
    sv_test::cornell_box box = sv_test::make_cornell_box();
    sv::scene_item item;
    cc::vector<sg::tlas_instance> instances;
    cc::vector<sv::material_permutation const*> hit_groups;
    sv::pt_light_table lights;
    sv::camera camera;
};

[[nodiscard]] cornell_scene make_scene(sv::gpu_resource_manager& resources)
{
    auto s = cornell_scene{};
    s.item = resources.acquire_scene_item(sv_test::as_mesh("wavefront cornell", s.box.positions, s.box.materials));
    resources.wait_for_pending_uploads();
    auto const* const mesh = resources.meshes.get_ptr(s.item.mesh);
    REQUIRE(mesh != nullptr);
    auto const* const permutation = resources.shaders.find(s.item.shader_key);
    REQUIRE(permutation != nullptr);
    s.instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, s.item.mesh), .instance_id = 0, .hit_group_offset = 0});
    s.hit_groups.push_back(permutation);
    s.lights = sv_test::light_table_of(s.box.light);
    s.camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    s.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    return s;
}

[[nodiscard]] sv::shaders::tracer::frame_constants frame_of(cornell_scene const& s, u32 seed, u32 accum_frame, i32 spp)
{
    auto fc = sv::default_frame_constants();
    fc.camera = sv::camera_record_of(s.camera);
    fc.previous_camera = fc.camera;
    s.lights.describe_in(fc);
    fc.samples_per_pixel = spp;
    fc.max_bounces = k_max_bounces;
    fc.rng_seed = seed;
    fc.accum_frame = accum_frame;
    return fc;
}

[[nodiscard]] sg::texture_2d make_target(sg::context& ctx, tg::vec2i size = k_size)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
}

/// Records frames through `record` until it executes `frames` of them, then reads the target back after the last.
template <class Record>
cc::shared_async<cc::vector<tg::vec3f>> trace_frames(sg::context& ctx, sg::texture_2d const& target, int frames, Record record)
{
    auto const start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        auto const ready = record(*cmd, f) == sg::routine_outcome::executed;
        auto readback = sg::data_future<tg::vec4f>();
        if (ready && f == frames - 1)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (readback.is_valid())
        {
            auto const pixels = readback.try_get_data();
            REQUIRE(pixels.has_value());
            auto out = cc::vector<tg::vec3f>();
            for (auto const& p : pixels.value())
                out.push_back(tg::vec3f(p[0], p[1], p[2]));
            co_return out;
        }
        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - start < 60.0);
            sv_test::drive_ambient_work();
            continue;
        }
        ++f;
    }
    FAIL("no frame was traced");
    co_return {};
}

[[nodiscard]] f32 luminance_of(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// What one wavefront run of a single mesh is: where it is seen from, what lights it, and how long it runs.
struct wavefront_run
{
    sv::camera camera;
    sv::background environment;
    i32 max_bounces = 5;
    /// frames that keep every slot busy, then one that drains
    int frames = 24;
    int rounds = 12;
    int drain_rounds = 48;
    tg::vec2i size = k_size;
};

/// Traces `mesh` through the wavefront, every frame but the last regenerating, and reads the drained image back.
cc::shared_async<cc::vector<tg::vec3f>> trace_wavefront(sg::context& ctx,
                                                        sv::gpu_resource_manager& resources,
                                                        sv::mesh const& mesh,
                                                        wavefront_run run)
{
    auto const item = resources.acquire_scene_item(mesh);
    resources.wait_for_pending_uploads();
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh),
                                          .instance_id = 0,
                                          .hit_group_offset = 0,
                                          .opaque_override = !permutation->opacity.can_cut_out()});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);
    auto const background = sv::background_gpu::from(run.environment);

    auto const target = make_target(ctx, run.size);
    auto state = sv::wavefront_state::create(ctx, run.size, 4);
    co_return co_await trace_frames(
        ctx, target, run.frames,
        [&](sg::command_list& cmd, int f)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
            sv::place_instance_record(records.back(), instances[0], permutation->opacity);
            auto const bindless = resources.freeze();
            auto const trace = sv::pt_trace_desc{
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = instances,
                .output = target,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .hit_groups = hit_groups,
                .bindless = &bindless};
            auto fc = sv::default_frame_constants();
            fc.camera = sv::camera_record_of(run.camera);
            fc.previous_camera = fc.camera;
            fc.max_bounces = run.max_bounces;
            fc.rng_seed = 3u;
            auto const last = f == run.frames - 1;
            return sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                        .frame = fc,
                                                        .state = &state,
                                                        .rounds = last ? run.drain_rounds : run.rounds,
                                                        .restart = f == 0,
                                                        .regenerate = !last,
                                                        .generation = 1});
        });
}

/// A closed cube, wound so every face points outward.
[[nodiscard]] cc::vector<tg::pos3f> closed_cube(float h)
{
    auto const corners = cc::array<tg::pos3f>{
        tg::pos3f(-h, -h, -h), tg::pos3f(+h, -h, -h), tg::pos3f(+h, +h, -h), tg::pos3f(-h, +h, -h),
        tg::pos3f(-h, -h, +h), tg::pos3f(+h, -h, +h), tg::pos3f(+h, +h, +h), tg::pos3f(-h, +h, +h),
    };
    auto const faces = cc::array<u32>{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                      3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    auto out = cc::vector<tg::pos3f>();
    for (auto const i : faces)
        out.push_back(corners[i]);
    return out;
}

[[nodiscard]] f32 block_mean(cc::span<tg::vec3f const> pixels, int bx, int by)
{
    auto sum = 0.0f;
    for (auto y = by * k_block; y < (by + 1) * k_block; ++y)
        for (auto x = bx * k_block; x < (bx + 1) * k_block; ++x)
            sum += luminance_of(pixels[y * k_size[0] + x]);
    return sum / f32(k_block * k_block);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::wavefront_routine - traces the megakernel's image of the Cornell box",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const scene = make_scene(resources);
    auto const background = sv::background_gpu::from(sv::background{});

    // The megakernel's mean of 32 frames of 8 samples.
    auto const reference_target = make_target(ctx);
    auto const reference = co_await trace_frames(
        ctx, reference_target, 32,
        [&](sg::command_list& cmd, int f)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
            sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
            auto const fc = frame_of(scene, u32(f) + 1u, u32(f), 8);
            auto const bindless = resources.freeze();
            return sv::pathtrace_routine::execute(
                cmd,
                {.frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer),
                 .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                 .instances = scene.instances,
                 .output = reference_target,
                 .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                 .lights = sv_test::upload_lights(cmd, scene.lights),
                 .hit_groups = scene.hit_groups,
                 .bindless = &bindless});
        });

    // The wavefront's: a restart, frames that keep every slot busy, and a last frame that drains what is still in flight.
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    constexpr int frames = 48;
    auto const traced = co_await trace_frames(
        ctx, target, frames,
        [&](sg::command_list& cmd, int f)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
            sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
            auto const bindless = resources.freeze();
            auto const trace = sv::pt_trace_desc{
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = scene.instances,
                .output = target,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = sv_test::upload_lights(cmd, scene.lights),
                .hit_groups = scene.hit_groups,
                .bindless = &bindless};
            auto const last = f == frames - 1;
            return sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                        .frame = frame_of(scene, 7u, 0u, 1),
                                                        .state = &state,
                                                        .rounds = last ? 8 : 6,
                                                        .restart = f == 0,
                                                        .regenerate = !last,
                                                        .generation = 1});
        });

    REQUIRE(reference.size() == traced.size());

    auto reference_mean = 0.0f;
    auto traced_mean = 0.0f;
    auto finite = true;
    for (auto i = isize(0); i < traced.size(); ++i)
    {
        reference_mean += luminance_of(reference[i]);
        traced_mean += luminance_of(traced[i]);
        finite = finite && tg::is_finite(traced[i][0]) && tg::is_finite(traced[i][1]) && tg::is_finite(traced[i][2]);
    }
    reference_mean /= f32(traced.size());
    traced_mean /= f32(traced.size());
    CHECK(finite);
    CHECK(traced_mean > 0.01f);
    CHECK(tg::abs(traced_mean - reference_mean) < 0.03f * reference_mean)
        .context(cc::format("image mean: wavefront {}, megakernel {}", traced_mean, reference_mean));

    // Block by block, against a tolerance the blocks' own noise sets: wide where the light is, narrow in the dark.
    auto worst = 0.0f;
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
        {
            auto const r = block_mean(reference, bx, by);
            auto const w = block_mean(traced, bx, by);
            auto const err = tg::abs(w - r) / (r + 0.05f);
            worst = cc::max(worst, err);
            CHECK(err < 0.25f).context(cc::format("block ({}, {}): wavefront {}, megakernel {}", bx, by, w, r));
        }
    CC_LOG_INFO("wavefront against megakernel: image means {} / {}, worst block {}", traced_mean, reference_mean, worst);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The pool's two invariants, read off the per-round live-path counts.
// A carrying frame refills every slot whose path finished, so every round sees every slot; that is what makes a frame's cost its rounds.
// A draining frame starts nothing, so its count never rises, and a scene with no medium empties within `max_bounces` rounds.
// A path takes one round per surface bounce, so whatever bounce it carried in at, none enters the drain's round at index `max_bounces` or later.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a carrying frame keeps every slot busy and a drain empties them",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const scene = make_scene(resources);
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);

    constexpr int carry_rounds = 6;
    constexpr int drain_rounds = 16;
    auto carried = sg::data_future<u32>();
    auto drained = sg::data_future<u32>();
    (void)co_await trace_frames(
        ctx, target, 2,
        [&](sg::command_list& cmd, int f)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
            sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
            auto const bindless = resources.freeze();
            auto const trace = sv::pt_trace_desc{
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = scene.instances,
                .output = target,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = sv_test::upload_lights(cmd, scene.lights),
                .hit_groups = scene.hit_groups,
                .bindless = &bindless};
            auto const carrying = f == 0;
            return sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                        .frame = frame_of(scene, 5u, 0u, 1),
                                                        .state = &state,
                                                        .rounds = carrying ? carry_rounds : drain_rounds,
                                                        .restart = carrying,
                                                        .regenerate = carrying,
                                                        .generation = 1,
                                                        .live_paths = carrying ? &carried : &drained});
        });

    auto const slots = u32(k_size[0] * k_size[1]);
    auto const carry = carried.try_get_data();
    REQUIRE(carry.has_value());
    REQUIRE(carry.value().size() == carry_rounds);
    for (auto r = 0; r < carry_rounds; ++r)
        CHECK(carry.value()[r] == slots).context(cc::format("carrying round {}", r));

    auto const drain = drained.try_get_data();
    REQUIRE(drain.has_value());
    REQUIRE(drain.value().size() == drain_rounds);
    CHECK(drain.value()[0] == slots);
    for (auto r = 1; r < drain_rounds; ++r)
        CHECK(drain.value()[r] <= drain.value()[r - 1]).context(cc::format("draining round {}", r));
    for (auto r = k_max_bounces; r < drain_rounds; ++r)
        CHECK(drain.value()[r] == 0u).context(cc::format("draining round {}, past every bounce", r));

    co_await cc::async_settled(sv::background_work(ctx));
}

// A still frame of one round starts a new sample in only the slots whose path finished last round, which is few of them.
// The primary depth is what drawings are depth-tested against, so it holds the estimate's, not the frame's: every covered pixel keeps its depth.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a still frame of one round keeps the primary depth its estimate found",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const scene = make_scene(resources);
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto const depth = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::r32_float,
         .width = k_size[0],
         .height = k_size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
    auto state = sv::wavefront_state::create(ctx, k_size, 4);

    // A restart that keeps every slot busy, then a still frame of a single round.
    auto restarted_depth = sg::data_future<f32>();
    auto still_depth = sg::data_future<f32>();
    (void)co_await trace_frames(
        ctx, target, 2,
        [&](sg::command_list& cmd, int f)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
            sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
            auto const bindless = resources.freeze();
            auto const trace = sv::pt_trace_desc{
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = scene.instances,
                .output = target,
                .primary_depth = depth,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = sv_test::upload_lights(cmd, scene.lights),
                .hit_groups = scene.hit_groups,
                .bindless = &bindless};
            auto fc = frame_of(scene, 9u, 0u, 1);
            fc.write_primary_depth = 1;
            fc.near_plane = f32(scene.camera.projection.near_plane);
            auto const outcome = sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                                      .frame = fc,
                                                                      .state = &state,
                                                                      .rounds = f == 0 ? 6 : 1,
                                                                      .restart = f == 0,
                                                                      .regenerate = true,
                                                                      .generation = 1});
            if (outcome == sg::routine_outcome::executed)
                (f == 0 ? restarted_depth : still_depth)
                    = sg::data_future<f32>(cmd.download.bytes_from_texture(depth.raw()));
            return outcome;
        });

    auto const restarted = restarted_depth.try_get_data();
    auto const still = still_depth.try_get_data();
    REQUIRE(restarted.has_value());
    REQUIRE(still.has_value());
    REQUIRE(restarted.value().size() == still.value().size());

    auto covered = isize(0);
    auto lost = isize(0);
    auto moved = isize(0);
    for (auto i = isize(0); i < still.value().size(); ++i)
    {
        if (restarted.value()[i] >= 1.0f)
            continue;
        ++covered;
        if (still.value()[i] >= 1.0f)
            ++lost;
        else if (still.value()[i] != restarted.value()[i])
            ++moved;
    }
    CHECK(covered > still.value().size() / 2).context(cc::format("{} of {} pixels covered", covered, still.value().size()));
    CHECK(lost == 0).context(cc::format("{} of {} covered pixels read the far plane", lost, covered));
    CHECK(moved == 0).context(cc::format("{} of {} covered pixels changed their depth", moved, covered));

    co_await cc::async_settled(sv::background_work(ctx));
}

// The furnace through the wavefront: a lossless object under a uniform environment returns the environment, which is what
// segment's medium walk and shade's refraction have to get right between them — see volumetric-furnace-test.cc for the megakernel's.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a lossless interior is invisible under a uniform environment",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    using binding = sv::material_attribute_binding;
    auto const environment = tg::vec3f(0.5f, 0.5f, 0.5f);
    auto const base = []
    {
        auto out = cc::vector<binding>();
        out.push_back(binding::of("specular_ior", 1.001f));
        out.push_back(binding::of("specular_roughness", 0.05f));
        out.push_back(binding::of("transmission_weight", 1.0f));
        return out;
    };
    struct furnace_case
    {
        cc::string_view name;
        cc::vector<binding> bindings;
        bool lossless = true;
    };
    auto cases = cc::vector<furnace_case>();
    cases.push_back({.name = "clear interior", .bindings = base()});
    {
        auto glass = cc::vector<binding>();
        glass.push_back(binding::of("specular_ior", 1.5f));
        glass.push_back(binding::of("specular_roughness", 0.05f));
        glass.push_back(binding::of("transmission_weight", 1.0f));
        cases.push_back({.name = "glass interior at index 1.5", .bindings = cc::move(glass)});
    }
    {
        auto scattering = base();
        scattering.push_back(binding::of("transmission_depth", 0.25f));
        scattering.push_back(binding::of("transmission_color", tg::vec3f(1, 1, 1)));
        scattering.push_back(binding::of("transmission_scatter", tg::vec3f(0.6f, 0.6f, 0.6f)));
        cases.push_back({.name = "scattering interior", .bindings = cc::move(scattering)});
    }
    {
        auto absorbing = base();
        absorbing.push_back(binding::of("transmission_depth", 0.25f));
        absorbing.push_back(binding::of("transmission_color", tg::vec3f(0.5f, 0.5f, 0.5f)));
        absorbing.push_back(binding::of("transmission_scatter", tg::vec3f(0.4f, 0.4f, 0.4f)));
        cases.push_back({.name = "absorbing interior", .bindings = cc::move(absorbing), .lossless = false});
    }

    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto camera = sv::camera{.position = tg::pos3d(0, 0, -1.9)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const expected = luminance_of(environment);

    for (auto const& c : cases)
    {
        auto const mesh = sv::mesh{.name = cc::format("wavefront {}", c.name),
                                   .geometry = sv::triangle_geometry::create_from_positions(closed_cube(0.6f)),
                                   .material = lib.acquire(sv::material::create(cc::string(c.name), type, c.bindings))};
        auto const pixels = co_await trace_wavefront(
            ctx, resources, mesh,
            {.camera = camera, .environment = sv::background::uniform(environment), .max_bounces = 12, .drain_rounds = 96});
        auto mean = 0.0f;
        for (auto const& p : pixels)
            mean += luminance_of(p);
        mean /= f32(pixels.size());
        if (c.lossless)
            CHECK(tg::abs(mean - expected) <= 0.06f * expected).context(c.name).dump("mean luminance", mean);
        else
        {
            CHECK(mean < expected * 0.9f).context(c.name).dump("mean luminance", mean);
            CHECK(mean > 0.0f).context(c.name).dump("mean luminance", mean);
        }
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// Cutout through the wavefront: extend and connect run the same generic opacity reader the megakernel's any-hits do.
// A black unlit quad half cut away in front of a white environment comes back half black, half white.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a cutout cuts where its opacity says", (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto positions = cc::vector<tg::pos3f>();
    for (auto const p : {tg::pos3f(-2, -2, 0), tg::pos3f(2, -2, 0), tg::pos3f(2, 2, 0), tg::pos3f(-2, -2, 0),
                         tg::pos3f(2, 2, 0), tg::pos3f(-2, 2, 0)})
        positions.push_back(p);
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::unlit).value();
    auto bindings = cc::vector<sv::material_attribute_binding>();
    bindings.push_back(sv::material_attribute_binding::of("color", tg::vec3f(0, 0, 0)));
    auto attributes = cc::vector<sv::mesh_attribute>();
    attributes.push_back(
        sv::mesh_attribute::create("opacity", sv::attribute_frequency::per_triangle, cc::array<f32>{0.0f, 1.0f}));
    auto const mesh = sv::mesh{.name = "wavefront cutout",
                               .geometry = sv::triangle_geometry::create_from_positions(positions),
                               .attributes = cc::move(attributes),
                               .material = lib.acquire(sv::material::create("wavefront cutout", type, bindings))};

    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const pixels = co_await trace_wavefront(
        ctx, resources, mesh,
        {.camera = camera, .environment = sv::background::uniform(tg::vec3f(1, 1, 1)), .max_bounces = 1, .frames = 2});

    auto kept = isize(0);
    auto cut = isize(0);
    for (auto const& p : pixels)
    {
        auto const l = luminance_of(p);
        if (tg::abs(l) < 0.02f)
            ++kept;
        else if (tg::abs(l - 1.0f) < 0.02f)
            ++cut;
    }
    auto const total = isize(pixels.size());
    // The pixels the diagonal runs through average jittered samples from both triangles, so they are neither; nothing else may be.
    CHECK(total - kept - cut <= isize(k_size[0])).context(cc::format("{} pixels neither kept nor cut", total - kept - cut));
    CHECK(kept > total * 4 / 10).context(cc::format("{} of {} kept", kept, total));
    CHECK(cut > total * 4 / 10).context(cc::format("{} of {} cut", cut, total));

    co_await cc::async_settled(sv::background_work(ctx));
}

// A coverage cutout through the wavefront: each sample tosses its own coin, even the samples one pixel finishes within one frame.
// A black unlit quad at half coverage in front of a white environment, one round per sample and every sample of the carrying frame
// drawn in that frame, so a coin shared by a pixel's samples would leave each pixel black or white rather than gray.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a coverage cutout tosses a coin per sample",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto positions = cc::vector<tg::pos3f>();
    for (auto const p : {tg::pos3f(-2, -2, 0), tg::pos3f(2, -2, 0), tg::pos3f(2, 2, 0), tg::pos3f(-2, -2, 0),
                         tg::pos3f(2, 2, 0), tg::pos3f(-2, 2, 0)})
        positions.push_back(p);
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::unlit).value();
    auto bindings = cc::vector<sv::material_attribute_binding>();
    bindings.push_back(sv::material_attribute_binding::of("color", tg::vec3f(0, 0, 0)));
    bindings.push_back(sv::material_attribute_binding::of("opacity", 0.5f));
    auto const mesh = sv::mesh{.name = "wavefront coverage",
                               .geometry = sv::triangle_geometry::create_from_positions(positions),
                               .material = lib.acquire(sv::material::create("wavefront coverage", type, bindings))};

    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto resources = sv::gpu_resource_manager::create(ctx);
    constexpr int samples = 32;
    auto const pixels = co_await trace_wavefront(ctx, resources, mesh,
                                                 {.camera = camera,
                                                  .environment = sv::background::uniform(tg::vec3f(1, 1, 1)),
                                                  .max_bounces = 1,
                                                  .frames = 2,
                                                  .rounds = samples});

    // At 33 samples a pixel's mean has a deviation near 0.09 about 0.5, so a pixel outside 0.1 to 0.9 is all but impossible.
    auto mixed = isize(0);
    auto mean = 0.0f;
    for (auto const& p : pixels)
    {
        auto const l = luminance_of(p);
        mean += l;
        if (l > 0.1f && l < 0.9f)
            ++mixed;
    }
    auto const total = isize(pixels.size());
    mean /= f32(total);
    CHECK(mixed > total * 95 / 100).context(cc::format("{} of {} pixels mix kept and cut samples", mixed, total));
    CHECK(tg::abs(mean - 0.5f) < 0.05f).dump("mean luminance", mean);

    co_await cc::async_settled(sv::background_work(ctx));
}

// An image past 65535 groups of 64 threads, which no backend dispatches on one axis: 4096 x 1040 is 4.26 Mpx.
// An unlit quad fills the view, so every stage but connect runs over more slots than one row of groups holds.
// A slot past the first row that no stage reached reads as a pixel that is not the quad's color.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - an image past one row of groups is traced to its last pixel",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    constexpr auto size = tg::vec2i(4096, 1040);
    static_assert(isize(size[0]) * isize(size[1]) > isize(65535) * 64, "the image needs a second row of groups");

    auto positions = cc::vector<tg::pos3f>();
    for (auto const p : {tg::pos3f(-40, -40, 0), tg::pos3f(40, -40, 0), tg::pos3f(40, 40, 0), tg::pos3f(-40, -40, 0),
                         tg::pos3f(40, 40, 0), tg::pos3f(-40, 40, 0)})
        positions.push_back(p);
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::unlit).value();
    auto bindings = cc::vector<sv::material_attribute_binding>();
    bindings.push_back(sv::material_attribute_binding::of("color", tg::vec3f(0.25f, 0.5f, 0.75f)));
    auto const mesh = sv::mesh{.name = "wavefront wide quad",
                               .geometry = sv::triangle_geometry::create_from_positions(positions),
                               .material = lib.acquire(sv::material::create("wavefront wide quad", type, bindings))};

    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    camera.projection.aspect_ratio = f64(size[0]) / f64(size[1]);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const pixels = co_await trace_wavefront(ctx, resources, mesh,
                                                 {.camera = camera,
                                                  .environment = sv::background::uniform(tg::vec3f(0, 0, 0)),
                                                  .max_bounces = 1,
                                                  .frames = 2,
                                                  .rounds = 2,
                                                  .drain_rounds = 4,
                                                  .size = size});
    REQUIRE(pixels.size() == isize(size[0]) * isize(size[1]));

    // The quad's color is whatever the first pixel holds; every other one, the last row's included, must hold it too.
    auto const expected = pixels[0];
    auto wrong = isize(0);
    auto first_wrong = isize(-1);
    for (auto i = isize(0); i < pixels.size(); ++i)
    {
        auto const d = pixels[i] - expected;
        if (tg::abs(d[0]) + tg::abs(d[1]) + tg::abs(d[2]) <= 1e-3f)
            continue;
        if (first_wrong < 0)
            first_wrong = i;
        ++wrong;
    }
    CHECK(luminance_of(expected) > 0.1f).dump("first pixel's luminance", luminance_of(expected));
    CHECK(wrong == 0).context(cc::format("{} pixels differ from the first, the first at slot {}", wrong, first_wrong));

    co_await cc::async_settled(sv::background_work(ctx));
}

#if SV_TEST_HAS_VULKAN
// The wavefront under vulkan's synchronization validation, which reports a hazard between two dispatches by the accesses it names.
// sg infers every barrier the round's stages need from their footprints, and this is the oracle that sees one missing:
// a reported hazard is logged as an error, and fails the test through the log rule.
// A scattering interior and the Cornell box between them run every stage, the medium walk and the shadow rays included.
ASYNC_TEST("sv vulkan - the wavefront raises no synchronization hazard")
{
    auto created = sg::create_vulkan_context({.enable_validation_layers = true, .enable_sync_validation = true});
    if (created.has_error())
        SKIP("no vulkan device");
    auto& ctx = *created.value();
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("the wavefront tracer needs inline ray queries");
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches SPIR-V to build the path-tracing shaders");

    using binding = sv::material_attribute_binding;
    auto bindings = cc::vector<binding>();
    bindings.push_back(binding::of("specular_ior", 1.001f));
    bindings.push_back(binding::of("specular_roughness", 0.05f));
    bindings.push_back(binding::of("transmission_weight", 1.0f));
    bindings.push_back(binding::of("transmission_depth", 0.25f));
    bindings.push_back(binding::of("transmission_color", tg::vec3f(1, 1, 1)));
    bindings.push_back(binding::of("transmission_scatter", tg::vec3f(0.6f, 0.6f, 0.6f)));
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto camera = sv::camera{.position = tg::pos3d(0, 0, -1.9)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
    auto const environment = tg::vec3f(0.5f, 0.5f, 0.5f);

    auto resources = sv::gpu_resource_manager::create(ctx);
    {
        auto const cube = sv::mesh{.name = "vulkan wavefront cube",
                                   .geometry = sv::triangle_geometry::create_from_positions(closed_cube(0.6f)),
                                   .material = lib.acquire(sv::material::create("vulkan wavefront", type, bindings))};
        auto const pixels = co_await trace_wavefront(
            ctx, resources, cube,
            {.camera = camera, .environment = sv::background::uniform(environment), .max_bounces = 12, .drain_rounds = 96});
        auto mean = 0.0f;
        for (auto const& p : pixels)
            mean += luminance_of(p);
        mean /= f32(pixels.size());
        CHECK(tg::abs(mean - luminance_of(environment)) <= 0.06f * luminance_of(environment)).dump("mean luminance", mean);
    }
    {
        auto const box = sv_test::make_cornell_box();
        auto box_camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
        box_camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
        auto const pixels = co_await trace_wavefront(
            ctx, resources, sv_test::as_mesh("vulkan wavefront box", box.positions, box.materials),
            {.camera = box_camera, .environment = sv::background::uniform(environment)});
        auto finite = true;
        for (auto const& p : pixels)
            finite = finite && tg::is_finite(p[0]) && tg::is_finite(p[1]) && tg::is_finite(p[2]);
        CHECK(finite);
    }

    co_await cc::async_settled(sv::background_work(ctx));
    CHECK(!ctx.is_device_lost());
}
#endif
