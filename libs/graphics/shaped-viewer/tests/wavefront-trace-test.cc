#include "viewer_test_env.hh"
#include "wavefront_test_support.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/container/array.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs, tg::sqrt

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

// The wavefront tracer against the megakernel it replaces, on the same scene and the same estimator.
//
// Both integrate one path per sample, five bounces deep with no roulette, so their means agree up to Monte Carlo noise.
// The comparison is of block means rather than of pixels, which at a few hundred samples each are still noise.
// ReSTIR DI over emissive lamps is restir-di-emitter-test.cc's, on the harness the two files share in wavefront_test_support.hh.

namespace
{
using namespace cc::primitive_defines;
using namespace sv_test::wavefront;

constexpr int k_block = 4;

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
                                                        .regenerate = !last});
        });
}

/// Traces the Cornell box at `fill`: a restart that drains, carrying frames, and a last frame that drains again.
/// The restart leaves every slot free, so the second frame's first round holds exactly the samples `fill` started.
/// `live_paths`, where set, receives that second frame's per-round counts.
cc::shared_async<cc::vector<tg::vec3f>> trace_cornell_at_fill(sg::context& ctx,
                                                              sv::gpu_resource_manager& resources,
                                                              cornell_scene const& scene,
                                                              f32 fill,
                                                              int frames,
                                                              sg::data_future<u32>* live_paths)
{
    REQUIRE(frames >= 3);
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto first_drain = sg::data_future<u32>();
    auto last_drain = sg::data_future<u32>();
    auto image = co_await trace_frames(
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
            auto const draining = f == 0 || last;
            auto* const live = f == 0 ? &first_drain : f == 1 ? live_paths : last ? &last_drain : nullptr;
            return sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                        .frame = frame_of(scene, 7u, 0u, 1),
                                                        .state = &state,
                                                        .rounds = draining ? drain_rounds_of(k_max_bounces) : 6,
                                                        .restart = f == 0,
                                                        .regenerate = !draining,
                                                        .fill = fill,
                                                        .live_paths = live});
        });
    check_drained(first_drain, "the restart");
    check_drained(last_drain, "the last frame");
    co_return image;
}

/// One drained frame of the Cornell box through the wavefront, ReSTIR's records kept where `restir` is set.
/// `read` downloads whatever the test reads back of `state`, on the same list, after the frame.
template <class Read>
cc::shared_async<cc::vector<tg::vec3f>> drain_cornell(sg::context& ctx,
                                                      sv::gpu_resource_manager& resources,
                                                      cornell_scene const& scene,
                                                      sv::wavefront_state& state,
                                                      bool restir,
                                                      Read read)
{
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto drained = sg::data_future<u32>();
    auto image = co_await trace_frames(
        ctx, target, 1,
        [&](sg::command_list& cmd, int)
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
            auto const outcome = sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                                      .frame = frame_of(scene, 7u, 0u, 1),
                                                                      .state = &state,
                                                                      .rounds = drain_rounds_of(k_max_bounces),
                                                                      .restart = true,
                                                                      .regenerate = false,
                                                                      .restir = restir,
                                                                      .live_paths = &drained});
            if (outcome == sg::routine_outcome::executed)
                read(cmd);
            return outcome;
        });
    check_drained(drained, "the drained frame");
    co_return image;
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
                                                        .regenerate = !last});
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
                                                                      .regenerate = true});
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

// A fill below 1 starts that share of the free slots, by a draw from each slot's own stream.
// It lowers what a round holds, which is how a still frame fits a budget, and must leave the image's mean where a full fill puts it.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a partial fill starts its share of the free slots and keeps the image's "
                     "mean",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // A quarter fill finishes fewer samples per frame, so it runs twice the frames to reach a comparable count.
    constexpr int frames = 48;
    constexpr f32 fill = 0.25f;
    auto full_live = sg::data_future<u32>();
    auto partial_live = sg::data_future<u32>();
    auto const full = co_await trace_cornell_at_fill(ctx, resources, scene, 1.0f, frames, &full_live);
    auto const partial = co_await trace_cornell_at_fill(ctx, resources, scene, fill, 2 * frames, &partial_live);

    auto const slots = u32(k_size[0] * k_size[1]);
    auto const full_counts = full_live.try_get_data();
    REQUIRE(full_counts.has_value());
    CHECK(full_counts.value()[0] == slots);

    // Each free slot starts with probability `fill`, so the first round's count is binomial: 256 of 1024, within five deviations.
    auto const partial_counts = partial_live.try_get_data();
    REQUIRE(partial_counts.has_value());
    auto const expected = f32(slots) * fill;
    auto const deviation = tg::sqrt(f32(slots) * fill * (1.0f - fill));
    CHECK(tg::abs(f32(partial_counts.value()[0]) - expected) < 5.0f * deviation)
        .context(cc::format("first round: {} live of {}, expected ~{}", partial_counts.value()[0], slots, expected));

    auto const full_mean = mean_luminance_of(full);
    auto const partial_mean = mean_luminance_of(partial);
    CHECK(full_mean > 0.01f);
    CHECK(tg::is_finite(partial_mean));
    CHECK(tg::abs(partial_mean - full_mean) < 0.03f * full_mean)
        .context(cc::format("image mean: fill {} gives {}, a full fill {}", fill, partial_mean, full_mean));
    CC_LOG_INFO("wavefront at fill {}: first round {} live of {}, image means {} / {} at a full fill", fill,
                partial_counts.value()[0], slots, partial_mean, full_mean);

    co_await cc::async_settled(sv::background_work(ctx));
}

namespace
{
/// One leg of a run toward a sample target: `frames` carrying frames of `rounds` rounds at `fill`, none past `target`.
struct target_leg
{
    u32 target = 0;
    int frames = 0;
    int rounds = 6;
    f32 fill = 1.0f;
};

/// What a run toward a sample target leaves: the drained image, every pixel's sample count, and the floor and shortfall read back.
struct target_run
{
    cc::vector<tg::vec3f> image;
    cc::vector<f32> counts;
    u32 floor = 0;
    u32 short_pixels = 0;
};

/// The Cornell box with a veil inside it: a square of openpbr at half opacity, its own instance, hung between the lamp and the floor.
/// Half opacity rather than a cutoff, so every ray crossing it, a camera ray or a shadow ray, draws its coverage.
struct veiled_scene
{
    cornell_scene box;
    sv::scene_item veil;
};

[[nodiscard]] veiled_scene make_veiled_scene(sv::gpu_resource_manager& resources)
{
    auto out = veiled_scene{.box = make_scene(resources)};
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto bindings = cc::vector<sv::material_attribute_binding>();
    bindings.push_back(sv::material_attribute_binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f)));
    bindings.push_back(sv::material_attribute_binding::of("opacity", 0.5f));
    auto const material = lib.acquire(sv::material::create("wavefront veil", type, cc::move(bindings)));

    // Level at y = 0.5, under the lamp (y = 0.998, |x|, |z| <= 0.35) and above the tall block (top at y = 0.25), wider than the lamp.
    // So the shadow rays carrying the box's direct light cross it, which a veil outside the box would not: no light reaches it from there,
    // and the black background gives an escaping ray nothing to carry either.
    // A shadow ray seeded by the frame rather than by its path would draw other coverage in the sparse run, and the image would move.
    // Camera rays toward the ceiling cross it too.
    auto positions = cc::vector<tg::pos3f>();
    positions.push_back(tg::pos3f(-0.6f, 0.5f, -0.6f));
    positions.push_back(tg::pos3f(0.6f, 0.5f, -0.6f));
    positions.push_back(tg::pos3f(0.6f, 0.5f, 0.6f));
    positions.push_back(tg::pos3f(-0.6f, 0.5f, -0.6f));
    positions.push_back(tg::pos3f(0.6f, 0.5f, 0.6f));
    positions.push_back(tg::pos3f(-0.6f, 0.5f, 0.6f));
    out.veil = resources.acquire_scene_item({.name = "wavefront veil",
                                             .geometry = sv::triangle_geometry::create_from_positions(positions),
                                             .material = material});
    resources.wait_for_pending_uploads();
    auto const* const permutation = resources.shaders.find(out.veil.shader_key);
    REQUIRE(permutation != nullptr);
    REQUIRE(permutation->opacity.can_cut_out());
    out.box.instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, out.veil.mesh),
                                                  .instance_id = 1,
                                                  .hit_group_offset = 2,
                                                  .opaque_override = false});
    out.box.hit_groups.push_back(permutation);
    return out;
}

/// Traces the Cornell box toward sample targets: a restart, every leg's carrying frames in turn, then a drain that reads the state back.
/// The restart is at seed 7 in every run, so each pixel's paths start from the same stream.
/// Each carrying frame draws its fill against a seed of its own, `seed_base` plus its index, as a view's frames do.
/// `veil`, where set, is the second instance `scene` lists.
cc::shared_async<target_run> trace_to_target(sg::context& ctx,
                                             sv::gpu_resource_manager& resources,
                                             cornell_scene const& scene,
                                             cc::span<target_leg const> legs,
                                             u32 seed_base,
                                             sv::scene_item const* veil = nullptr)
{
    REQUIRE(!legs.empty());
    auto frames = 2;
    for (auto const& leg : legs)
        frames += leg.frames;

    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto tallies = sg::data_future<sv::shaders::tracer::pixel_tally>();
    auto floor = sg::data_future<u32>();
    auto out = target_run{};
    out.image = co_await trace_frames(
        ctx, target, frames,
        [&](sg::command_list& cmd, int f)
        {
            // The leg frame `f` belongs to; the restart takes the first leg's target and the drain the last's.
            auto leg = legs[0];
            auto first = 1;
            for (auto const& l : legs)
            {
                leg = l;
                if (f < first + l.frames)
                    break;
                first += l.frames;
            }
            auto const restart = f == 0;
            auto const last = f == frames - 1;
            auto const draining = restart || last;

            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
            sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
            if (veil != nullptr)
            {
                records.push_back(resources.describe_instance(cmd, veil->mesh, veil->instance));
                sv::place_instance_record(records.back(), scene.instances[1], scene.hit_groups[1]->opacity);
            }
            auto const bindless = resources.freeze();
            auto const trace = sv::pt_trace_desc{
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = scene.instances,
                .output = target,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = sv_test::upload_lights(cmd, scene.lights),
                .hit_groups = scene.hit_groups,
                .bindless = &bindless};
            auto const seed = restart ? 7u : seed_base + u32(f);
            auto const outcome
                = sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                       .frame = frame_of(scene, seed, 0u, 1),
                                                       .state = &state,
                                                       .rounds = draining ? drain_rounds_of(k_max_bounces) : leg.rounds,
                                                       .restart = restart,
                                                       .regenerate = !draining,
                                                       .fill = draining ? 1.0f : leg.fill,
                                                       .sample_target = leg.target,
                                                       .sample_floor = last ? &floor : nullptr});
            if (last && outcome == sg::routine_outcome::executed)
                tallies = cmd.download.data_from_buffer(state.tallies);
            return outcome;
        });

    auto const read_tallies = tallies.try_get_data();
    REQUIRE(read_tallies.has_value());
    for (auto const& t : read_tallies.value())
        out.counts.push_back(t.count);
    auto const read_floor = floor.try_get_data();
    REQUIRE(read_floor.has_value());
    REQUIRE(read_floor.value().size() == 2);
    out.floor = read_floor.value()[0];
    out.short_pixels = read_floor.value()[1];
    co_return out;
}

/// Checks that every pixel of `run` holds exactly `target` samples, and that its floor read back says so.
void check_at_target(target_run const& run, u32 target, cc::string_view name)
{
    auto off = 0;
    for (auto const c : run.counts)
        off += c == f32(target) ? 0 : 1;
    CHECK(off == 0).context(cc::format("{}: {} pixels do not hold {} samples", name, off, target));
    CHECK(run.floor == target).context(cc::format("{}: the floor read back {}", name, run.floor));
    CHECK(run.short_pixels == 0).context(cc::format("{}: {} pixels read back as short", name, run.short_pixels));
}

/// The largest difference between two images, relative to the pixel's own value.
[[nodiscard]] f32 worst_difference(cc::span<tg::vec3f const> a, cc::span<tg::vec3f const> b)
{
    REQUIRE(a.size() == b.size());
    auto worst = 0.0f;
    for (auto i = isize(0); i < a.size(); ++i)
        for (auto c = 0; c < 3; ++c)
            worst = cc::max(worst, tg::abs(a[i][c] - b[i][c]) / (1.0f + tg::abs(b[i][c])));
    return worst;
}
} // namespace

// A sample target stops every pixel where a capture asks it to: a free slot whose pixel holds it starts nothing.
// The floor the routine reads back is what the view converges on, so it must say the same.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a sample target stops every pixel at it, and the floor reads it back",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // Six rounds finish every path of the box, so each carrying frame adds a sample to every pixel below the target.
    constexpr u32 target = 6;
    auto const legs = cc::array<target_leg>{{.target = target, .frames = 12, .rounds = 6, .fill = 1.0f}};
    auto const run = co_await trace_to_target(ctx, resources, scene, legs, 100u);
    check_at_target(run, target, "a full fill");

    co_await cc::async_settled(sv::background_work(ctx));
}

// What makes a capture the same image whatever GPU takes it: a pixel's k-th sample traces the same path however it got there.
// Its paths continue one stream from sample to sample, and the fill draws from a stream of its own, so neither the
// fill nor the frames' split of the rounds — which is all a frame budget changes — moves a single sample.
// A target raised part way carries on from the samples the pixels hold, which is what a capture asking for more does.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a pixel's samples are the same whatever the fill and the frames' rounds",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    constexpr u32 target = 8;
    auto const full_legs = cc::array<target_leg>{{.target = target, .frames = 16, .rounds = 6, .fill = 1.0f}};
    // A third of the free slots a frame and two rounds: a sample takes several frames, so far more of them, every one seeded apart.
    auto const sparse_legs = cc::array<target_leg>{{.target = target, .frames = 160, .rounds = 2, .fill = 0.3f}};
    auto const raised_legs = cc::array<target_leg>{{.target = target / 2, .frames = 12, .rounds = 3, .fill = 1.0f},
                                                   {.target = target, .frames = 16, .rounds = 3, .fill = 1.0f}};

    auto const full = co_await trace_to_target(ctx, resources, scene, full_legs, 100u);
    auto const sparse = co_await trace_to_target(ctx, resources, scene, sparse_legs, 5000u);
    auto const raised = co_await trace_to_target(ctx, resources, scene, raised_legs, 9000u);
    check_at_target(full, target, "a full fill");
    check_at_target(sparse, target, "a sparse fill");
    check_at_target(raised, target, "a raised target");

    // The same samples summed in the same order, so the same floats; the tolerance only forgives a device that fuses differently.
    auto const sparse_worst = worst_difference(sparse.image, full.image);
    auto const raised_worst = worst_difference(raised.image, full.image);
    CHECK(sparse_worst <= 1e-5f).context(cc::format("a sparse fill against a full one: worst {}", sparse_worst));
    CHECK(raised_worst <= 1e-5f).context(cc::format("a raised target against one target: worst {}", raised_worst));

    // A stochastic cutout draws its coverage from the path's stream, so the frames' split moves none of its draws either.
    // Seeded by the frame instead, the sparse run's camera rays and shadow rays would cross the veil on other draws.
    auto const veiled = make_veiled_scene(resources);
    auto const veiled_full = co_await trace_to_target(ctx, resources, veiled.box, full_legs, 100u, &veiled.veil);
    auto const veiled_sparse = co_await trace_to_target(ctx, resources, veiled.box, sparse_legs, 5000u, &veiled.veil);
    check_at_target(veiled_full, target, "a full fill behind a cutout");
    check_at_target(veiled_sparse, target, "a sparse fill behind a cutout");
    auto const veiled_worst = worst_difference(veiled_sparse.image, veiled_full.image);
    CHECK(veiled_worst <= 1e-5f).context(cc::format("a cutout, sparse fill against a full one: worst {}", veiled_worst));

    co_await cc::async_settled(sv::background_work(ctx));
}

// The tally keeps the view depth of each pixel's first primary hit since the restart, and resolve applies the near plane.
// A drawing that starts needing depth on a still view sets the near plane without a restart, and must still see the geometry.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a primary depth first asked for on a still frame lands on the geometry",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    constexpr int frames = 6;
    constexpr f32 near_plane = 0.1f;
    auto depth_back = sg::data_future<f32>();
    (void)co_await trace_frames(
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
                .primary_depth = depth,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = sv_test::upload_lights(cmd, scene.lights),
                .hit_groups = scene.hit_groups,
                .bindless = &bindless};
            // Only the last frame asks for depth, as a drawing added to a still view does: the earlier ones leave the near plane 0.
            auto const last = f == frames - 1;
            auto fc = frame_of(scene, 7u, 0u, 1);
            if (last)
            {
                fc.write_primary_depth = 1;
                fc.near_plane = near_plane;
            }
            auto const outcome = sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                                      .frame = fc,
                                                                      .state = &state,
                                                                      .rounds = 2,
                                                                      .restart = f == 0,
                                                                      .regenerate = true,
                                                                      .fill = 0.25f});
            if (last && outcome == sg::routine_outcome::executed)
                depth_back = sg::data_future<f32>(cmd.download.bytes_from_texture(depth.raw()));
            return outcome;
        });

    // The center pixel sees the back wall, 4.4 in front of the camera.
    auto const texels = depth_back.try_get_data();
    REQUIRE(texels.has_value());
    auto const center = (k_size[1] / 2) * k_size[0] + k_size[0] / 2;
    auto const expected = 1.0f - near_plane / 4.4f;
    CHECK(tg::abs(texels.value()[center] - expected) < 2e-3f)
        .context(cc::format("center depth {}, the back wall's {}", texels.value()[center], expected));

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR GI's phase G0: the candidate recorded and shaded again, with reuse off (libs/graphics/shaped-viewer/docs/plans/restir-gi.md).
// Keeping the records rebases each path at its visible point, which must leave the mean where the plain paths put it.
// The re-shade evaluates the visible point's material toward the path's own `x_s`, which must give back the path's own estimate.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's re-shade of a drained frame gives back each path's own estimate",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    auto plain_state = sv::wavefront_state::create(ctx, k_size, 4);
    auto const plain = co_await drain_cornell(ctx, resources, scene, plain_state, false, [](sg::command_list&) {});

    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto tallies = sg::data_future<sv::shaders::tracer::pixel_tally>();
    auto candidates = sg::data_future<sv::shaders::tracer::packed_candidate>();
    auto results = sg::data_future<sv::shaders::tracer::restir_result>();
    auto const kept = co_await drain_cornell(ctx, resources, scene, state, true,
                                             [&](sg::command_list& cmd)
                                             {
                                                 tallies = cmd.download.data_from_buffer(state.tallies);
                                                 candidates = cmd.download.data_from_buffer(state.restir_candidates);
                                                 results = cmd.download.data_from_buffer(state.restir_results);
                                             });

    // The same seed draws the same paths: only the order the throughput is multiplied in differs.
    REQUIRE(plain.size() == kept.size());
    auto worst_mean = 0.0f;
    for (auto i = isize(0); i < plain.size(); ++i)
        for (auto c = 0; c < 3; ++c)
            worst_mean = cc::max(worst_mean, tg::abs(kept[i][c] - plain[i][c]) / (tg::abs(plain[i][c]) + 1e-3f));
    CHECK(worst_mean < 1e-3f).context(cc::format("the worst relative difference of the mean: {}", worst_mean));

    auto const t = tallies.try_get_data();
    auto const c = candidates.try_get_data();
    auto const r = results.try_get_data();
    REQUIRE(t.has_value());
    REQUIRE(c.has_value());
    REQUIRE(r.has_value());

    constexpr u32 candidate_flag = 4;
    constexpr u32 reconnectable_flag = 8;
    auto shaded = 0;
    auto reconnectable = 0;
    auto worst_shade = 0.0f;
    auto outside = 0;
    for (auto i = isize(0); i < t.value().size(); ++i)
    {
        auto const& cand = c.value()[i];
        if ((cand.flags & candidate_flag) != 0)
        {
            // The box spans [-1, 1] on every axis, so a secondary vertex outside it was not where the path went.
            auto const x = cand.x_s;
            outside += tg::abs(x[0]) > 1.01f || tg::abs(x[1]) > 1.01f || tg::abs(x[2]) > 1.01f ? 1 : 0;
            reconnectable += (cand.flags & reconnectable_flag) != 0 ? 1 : 0;
        }
        auto const& res = r.value()[i];
        if (res.valid == 0)
            continue;
        ++shaded;
        auto const own = t.value()[i].frame_sum / cc::max(t.value()[i].frame_count, 1.0f);
        for (auto ch = 0; ch < 3; ++ch)
            worst_shade = cc::max(worst_shade, tg::abs(res.radiance[ch] - own[ch]) / (tg::abs(own[ch]) + 1e-3f));
    }

    // Most of the image is the box's matte walls, whose continuations mostly stay inside it; some leave through the open front.
    auto const slots = isize(k_size[0]) * isize(k_size[1]);
    CHECK(shaded > slots / 3).context(cc::format("{} of {} pixels shaded again", shaded, slots));
    CHECK(reconnectable > slots / 3).context(cc::format("{} reconnectable candidates", reconnectable));
    CHECK(outside == 0);
    // The stored `L_i` is a half per channel, rounded to nearest, so the re-shade is within 2^-11 of the path's own estimate.
    CHECK(worst_shade < 1e-3f).context(cc::format("the worst relative difference of the re-shade: {}", worst_shade));
    CC_LOG_INFO("ReSTIR G0 on the box: {} of {} re-shaded, worst {} against the paths, worst mean {}", shaded, slots,
                worst_shade, worst_mean);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A ReSTIR frame that ends before every path does leaves its split paths to the next frame, which keeps no records.
// That frame must still put each path's light back together, or the mean loses `T_1` and the visible point's direct light.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a path a ReSTIR frame leaves unfinished is recombined by the frame that "
                     "ends it",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // Two rounds, which leave most paths of the box alive, then a drain of the rest without ReSTIR.
    auto const run = [&](bool restir) -> cc::shared_async<cc::vector<tg::vec3f>>
    {
        auto const target = make_target(ctx);
        auto state = sv::wavefront_state::create(ctx, k_size, 4);
        co_return co_await trace_frames(
            ctx, target, 2,
            [&](sg::command_list& cmd, int f)
            {
                auto records = cc::vector<sv::shaders::tracer::instance_record>();
                records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
                sv::place_instance_record(records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
                auto const bindless = resources.freeze();
                auto const trace = sv::pt_trace_desc{.background = ctx.transient.create_buffer_from_pod(
                                                         cmd, background, sg::buffer_usage::readonly_buffer),
                                                     .instances = scene.instances,
                                                     .output = target,
                                                     .instance_table = ctx.transient.create_buffer_from_data(
                                                         cmd, records, sg::buffer_usage::readonly_buffer),
                                                     .lights = sv_test::upload_lights(cmd, scene.lights),
                                                     .hit_groups = scene.hit_groups,
                                                     .bindless = &bindless};
                return sv::wavefront_routine::execute(cmd, {.trace = trace,
                                                            .frame = frame_of(scene, 7u, 0u, 1),
                                                            .state = &state,
                                                            .rounds = f == 0 ? 2 : 16,
                                                            .restart = f == 0,
                                                            .regenerate = false,
                                                            .restir = restir && f == 0});
            });
    };
    auto const plain = co_await run(false);
    auto const split = co_await run(true);

    REQUIRE(plain.size() == split.size());
    auto worst = 0.0f;
    for (auto i = isize(0); i < plain.size(); ++i)
        for (auto c = 0; c < 3; ++c)
            worst = cc::max(worst, tg::abs(split[i][c] - plain[i][c]) / (tg::abs(plain[i][c]) + 1e-3f));
    CHECK(worst < 1e-3f).context(cc::format("the worst relative difference of the mean: {}", worst));

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR GI's phase G1: temporal reuse, on a still camera so every visible point reprojects onto itself.
// The box is matte everywhere, the Lambertian case reconnection is exact for.
// So with the reverse visibility traced the resampled frames average to the plain path tracer's image, and each is less noisy than one plain sample.
// Without it they keep a slight bias, which a looser bound measures rather than hides.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's temporal reuse keeps the box's image and cuts each frame's "
                     "noise",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // The reference: the plain tracer's converged mean, many samples a pixel.
    auto const reference = co_await trace_cornell_at_fill(ctx, resources, scene, 1.0f, 96, nullptr);
    auto const reference_mean = mean_luminance_of(reference);

    constexpr int frames = 48;
    constexpr int warm = 16;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const exact = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = true});
    auto const fast = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = false});

    // Unbiased with `exact`: the mean of the resampled frames is the plain image's, to the frames' noise.
    auto const exact_mean = mean_luminance_of(exact.mean);
    auto const fast_mean = mean_luminance_of(fast.mean);
    auto const plain_mean = mean_luminance_of(plain.mean);
    CHECK(tg::abs(plain_mean - reference_mean) < 0.03f * reference_mean);
    CHECK(tg::abs(exact_mean - plain_mean) < 0.01f * plain_mean)
        .context(cc::format("exact: {} against {} without reuse", exact_mean, plain_mean));
    CHECK(tg::abs(fast_mean - plain_mean) < 0.02f * plain_mean)
        .context(cc::format("default: {} against {} without reuse", fast_mean, plain_mean));

    // Block by block, against a tolerance the blocks' own noise sets, as the megakernel comparison does.
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
        {
            auto const r = block_mean(reference, bx, by);
            auto const e = block_mean(exact.mean, bx, by);
            CHECK(tg::abs(e - r) / (r + 0.05f) < 0.25f)
                .context(cc::format("block ({}, {}): exact {}, reference {}", bx, by, e, r));
        }

    // Reuse happens: with the camera still, the reservoirs fill toward the confidence cap, and without history they stay at 1.
    CHECK(exact.confidence > 10.0f).context(cc::format("mean confidence {}", exact.confidence));
    CHECK(plain.confidence <= 1.0f).context(cc::format("mean confidence without history {}", plain.confidence));

    // The variance falls: once the history has filled, a resampled frame is closer to the reference.
    // Only the indirect half resamples, so one next-event sample's noise stays in both, which bounds the gain on the whole image.
    auto plain_error = 0.0f;
    auto exact_error = 0.0f;
    for (auto f = warm; f < frames; ++f)
    {
        plain_error += rms_error(plain.frames[f], reference) / f32(frames - warm);
        exact_error += rms_error(exact.frames[f], reference) / f32(frames - warm);
    }
    CHECK(exact_error * 1.3f < plain_error)
        .context(cc::format("per-frame error: {} resampled, {} plain", exact_error, plain_error));
    CC_LOG_INFO("ReSTIR G1 on the box: means {} exact, {} default, {} plain, {} reference; per-frame error {} against "
                "{} plain",
                exact_mean, fast_mean, plain_mean, reference_mean, exact_error, plain_error);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The temporal pass reprojects each visible point through the camera of the frame its history was kept under.
// A panning camera told where it was keeps most of its history.
// Told that nothing moved, its previous camera is not the one the history was kept under, and it reuses none of it.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's history follows a panning camera through its reprojection",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // About two pixels a frame at this size, back and forth, so every frame moves and the box stays in view.
    constexpr f64 pan = 0.25;
    constexpr int frames = 12;
    auto const honest = co_await run_restir(ctx, resources, scene, frames, {.pan = pan, .honest_motion = true});
    auto const blind = co_await run_restir(ctx, resources, scene, frames, {.pan = pan, .honest_motion = false});
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false, .pan = pan});

    CHECK(honest.confidence > 5.0f).context(cc::format("mean confidence {} panning", honest.confidence));

    // The moving image keeps the plain one's brightness: reuse across motion moves samples, it does not make light.
    auto const honest_mean = mean_luminance_of(honest.mean);
    auto const plain_mean = mean_luminance_of(plain.mean);
    CHECK(tg::abs(honest_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("moving: {} reusing, {} without", honest_mean, plain_mean));
    CHECK(blind.confidence <= 1.0f)
        .context(cc::format("mean confidence {} told the motion, {} told nothing moved", honest.confidence,
                            blind.confidence));
    CC_LOG_INFO("ReSTIR G1 panning: mean confidence {} with the motion, {} without", honest.confidence, blind.confidence);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A glossy back wall, which the candidates of the walls facing it reach and cannot reconnect to.
// The history never shifts such a sample, so its MIS weight must give it to the candidate whole, or reuse darkens what the wall reflects.
// Neighbors' samples on the wall are not reconnectable either, so spatial reuse must also leave it whole.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's reuse keeps the light a glossy wall reflects",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    // The back wall is the box's third quad, its triangles 4 and 5.
    auto box = sv_test::make_cornell_box();
    auto const glossy = sv_test::pbr_material{.base_color = tg::vec3f(0.9f, 0.9f, 0.9f),
                                              .metallic = 1.0f,
                                              .roughness = 0.1f,
                                              .emissive = tg::vec3f(0, 0, 0)};
    box.materials[4] = glossy;
    box.materials[5] = glossy;

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const scene = make_scene(resources, cc::move(box));

    constexpr int frames = 48;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const exact = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = true});

    auto const spatial
        = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = true, .spatial = true});

    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const exact_mean = mean_luminance_of(exact.mean);
    auto const spatial_mean = mean_luminance_of(spatial.mean);
    CHECK(tg::abs(exact_mean - plain_mean) < 0.015f * plain_mean)
        .context(cc::format("glossy back wall: {} reusing, {} without", exact_mean, plain_mean));
    CHECK(tg::abs(spatial_mean - plain_mean) < 0.015f * plain_mean)
        .context(cc::format("glossy back wall: {} reusing neighbors too, {} without", spatial_mean, plain_mean));
    CC_LOG_INFO("ReSTIR with a glossy wall: means {} temporal, {} with neighbors too, {} without; confidence {}",
                exact_mean, spatial_mean, plain_mean, exact.confidence);

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR GI's phase G2: spatial reuse on top of the temporal, each pixel resampling two neighbors' reservoirs of the same frame.
// Its mean is pinned at 12 pixels, a third of the image, so the shifts are long enough that a wrong Jacobian or MIS weight would move it.
// With the reverse visibility traced the frames still average to the plain image, the box's two blocks occluding many of the shifts.
// So is its mean at the image's own radius, the 2-pixel floor here, with the reverse visibility traced and without it, which darkens.
// Its noise is measured at that radius, on the indirect half, which is what resamples: on top of the temporal reuse neighbors cut it further.
// Under --thorough the plain reference is four times as long, and neighbors alone are measured to cut it too.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's spatial reuse keeps the box's image and cuts each frame's noise "
                     "further",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    constexpr int frames = 48;
    constexpr int warm = 16;
    constexpr i32 wide = 12;
    auto const thorough = nx::is_thorough();
    // Under --thorough the plain run is four times as long.
    auto const plain = co_await run_restir(ctx, resources, scene, thorough ? 4 * frames : frames, {.history = false});
    auto const spatial
        = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = true, .spatial = true});
    auto const far = co_await run_restir(ctx, resources, scene, frames,
                                         {.history = true, .exact = true, .spatial = true, .radius = wide});
    auto const fast = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true});

    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const far_mean = mean_luminance_of(far.mean);
    auto const spatial_mean = mean_luminance_of(spatial.mean);
    auto const fast_mean = mean_luminance_of(fast.mean);
    CHECK(tg::abs(far_mean - plain_mean) < 0.01f * plain_mean)
        .context(cc::format("exact, 12 pixels: {} against {} without reuse", far_mean, plain_mean));
    CHECK(tg::abs(spatial_mean - plain_mean) < 0.01f * plain_mean)
        .context(cc::format("exact: {} against {} without reuse", spatial_mean, plain_mean));
    CHECK(tg::abs(fast_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("default: {} against {} without reuse", fast_mean, plain_mean));
    CC_LOG_INFO("ReSTIR G2 on the box: means {} exact, {} at 12 pixels, {} default, {} plain", spatial_mean, far_mean,
                fast_mean, plain_mean);

    // The runs share per-frame seeds, so the default run's 48 plain frames are as good a reference for an ordering as four times as many.
    auto const temporal = co_await run_restir(ctx, resources, scene, frames, {.history = true, .exact = true});
    auto temporal_error = 0.0f;
    auto spatial_error = 0.0f;
    for (auto f = warm; f < frames; ++f)
    {
        temporal_error += rms_error(temporal.indirect_frames[f], plain.indirect_mean) / f32(frames - warm);
        spatial_error += rms_error(spatial.indirect_frames[f], plain.indirect_mean) / f32(frames - warm);
    }
    CHECK(spatial_error < temporal_error)
        .context(
            cc::format("indirect per-frame error: {} reusing both, {} temporal alone", spatial_error, temporal_error));
    CC_LOG_INFO("ReSTIR G2 indirect per-frame error: {} both, {} temporal alone", spatial_error, temporal_error);

    if (thorough)
    {
        auto const alone
            = co_await run_restir(ctx, resources, scene, frames, {.history = false, .exact = true, .spatial = true});
        auto plain_error = 0.0f;
        auto alone_error = 0.0f;
        for (auto f = warm; f < frames; ++f)
        {
            plain_error += rms_error(plain.indirect_frames[f], plain.indirect_mean) / f32(frames - warm);
            alone_error += rms_error(alone.indirect_frames[f], plain.indirect_mean) / f32(frames - warm);
        }
        CHECK(alone_error * 1.2f < plain_error)
            .context(
                cc::format("indirect per-frame error: {} reusing neighbors alone, {} plain", alone_error, plain_error));
        CC_LOG_INFO("ReSTIR G2 indirect per-frame error: {} neighbors alone, {} plain", alone_error, plain_error);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR's DI track, phase D0: next-event estimation picks a light in proportion to its power, in both tracers.
// The box gets a second ceiling light eight times dimmer than its own, which uniform selection gives half of every sample.
// Picking by power leaves the image's mean where uniform selection, the known-good estimator, puts it, and brings each frame closer to it.
// The megakernel picks through the same function and weighs a light it hits through its own, so its mean is pinned too.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - picking lights by power keeps the image and cuts its noise",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    auto const base = make_scene(resources);

    auto const& lamp = base.box.light;
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    records.push_back(base.lights.records[0]);
    records.push_back({.position = lamp.center + tg::vec3f(0, 0, 0.5f),
                       .path = u32(sv::light_path::area),
                       .u = tg::vec3f(lamp.half_x, 0, 0),
                       .area = 4.0f * lamp.half_x * lamp.half_z,
                       .v = tg::vec3f(0, 0, lamp.half_z),
                       .emission = lamp.emission * 0.125f,
                       .normal = tg::vec3f(0, -1, 0),
                       .cone_offset = 1,
                       .link_mask = ~0u});
    auto uniform = base;
    uniform.lights = sv::pt_light_table::grouped(records, sv::light_selection::uniform);
    auto powered = base;
    powered.lights = sv::pt_light_table::grouped(records, sv::light_selection::power);

    constexpr int frames = 48;
    auto const by_uniform = co_await run_restir(ctx, resources, uniform, frames, {.history = false});
    auto const by_power = co_await run_restir(ctx, resources, powered, frames, {.history = false});
    auto const uniform_mean = mean_luminance_of(by_uniform.mean);
    auto const power_mean = mean_luminance_of(by_power.mean);
    // The lamp's jittered edge makes the image mean noisier than its pixel count suggests, so the bound is the other tests' 3%.
    CHECK(tg::abs(power_mean - uniform_mean) < 0.03f * uniform_mean)
        .context(cc::format("by power: {} against {} uniform", power_mean, uniform_mean));

    // Under --thorough each frame is measured against a long reference run on seeds of its own, and otherwise against the two runs' mean.
    auto const thorough = nx::is_thorough();
    auto reference = cc::vector<tg::vec3f>();
    if (thorough)
        reference = (co_await run_restir(ctx, resources, uniform, 4 * frames, {.history = false, .seeds = 1})).mean;
    else
        for (auto i = isize(0); i < by_uniform.mean.size(); ++i)
            reference.push_back((by_uniform.mean[i] + by_power.mean[i]) * 0.5f);
    auto uniform_error = 0.0f;
    auto power_error = 0.0f;
    for (auto f = 0; f < frames; ++f)
    {
        uniform_error += rms_error(by_uniform.frames[f], reference) / f32(frames);
        power_error += rms_error(by_power.frames[f], reference) / f32(frames);
    }
    CHECK(power_error < uniform_error)
        .context(cc::format("per-frame error: {} by power, {} uniform", power_error, uniform_error));
    CC_LOG_INFO("D0 through the wavefront: means {} by power, {} uniform; per-frame error {} by power, {} uniform",
                power_mean, uniform_mean, power_error, uniform_error);

    // The megakernel, 16 frames of 8 samples each way.
    auto const megakernel_mean = [&](cornell_scene const& scene) -> cc::shared_async<f32>
    {
        auto const target = make_target(ctx);
        auto const background = sv::background_gpu::from(sv::background{});
        auto const image = co_await trace_frames(
            ctx, target, 16,
            [&](sg::command_list& cmd, int f)
            {
                auto instance_records = cc::vector<sv::shaders::tracer::instance_record>();
                instance_records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
                sv::place_instance_record(instance_records.back(), scene.instances[0], scene.hit_groups[0]->opacity);
                auto const fc = frame_of(scene, u32(f) + 1u, u32(f), 8);
                auto const bindless = resources.freeze();
                return sv::pathtrace_routine::execute(
                    cmd, {.frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer),
                          .background
                          = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                          .instances = scene.instances,
                          .output = target,
                          .instance_table = ctx.transient.create_buffer_from_data(cmd, instance_records,
                                                                                  sg::buffer_usage::readonly_buffer),
                          .lights = sv_test::upload_lights(cmd, scene.lights),
                          .hit_groups = scene.hit_groups,
                          .bindless = &bindless});
            });
        co_return mean_luminance_of(image);
    };
    auto const mk_uniform = co_await megakernel_mean(uniform);
    auto const mk_power = co_await megakernel_mean(powered);
    CHECK(tg::abs(mk_power - mk_uniform) < 0.03f * mk_uniform)
        .context(cc::format("megakernel: {} by power, {} uniform", mk_power, mk_uniform));
    CC_LOG_INFO("D0 on the box with a dim second light: megakernel means {} by power, {} uniform", mk_power, mk_uniform);

    if (thorough)
    {
        auto const reference_mean = mean_luminance_of(reference);
        CC_LOG_INFO("D0 reference mean: {}", reference_mean);
        CHECK(tg::abs(uniform_mean - reference_mean) < 0.03f * reference_mean)
            .context(cc::format("uniform: {} against {}", uniform_mean, reference_mean));
        CHECK(tg::abs(power_mean - reference_mean) < 0.03f * reference_mean)
            .context(cc::format("by power: {} against {}", power_mean, reference_mean));
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// Power knows nothing of distance: a light dim overall can be the one that lights the place a pixel sees.
// The box keeps its lamp and gains a point light in a floor corner, a tenth of the lamp's power.
// Picked by power alone it is drawn one time in 11 and weighted up when it is, so the corner is noise.
// The table mixes half uniform in, which brings that within reach of uniform selection and keeps the mean.
// Pure power is written into the same records by hand, as the table used to.
// Measured where the corner light carries most of the image, which a lamp-only run marks.
// A corner light dimmer still covers too few pixels to say anything, and over a whole frame the lamp lights most of,
// power alone can come out ahead; the corner is where starving a light shows.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a dim light near what it lights keeps a fair share of the choice",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    auto const base = make_scene(resources);

    auto const& lamp = base.box.light;
    auto const lamp_power = luminance_of(lamp.emission) * 4.0f * lamp.half_x * lamp.half_z * tg::pi<f32>;
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    records.push_back(base.lights.records[0]);
    records.push_back(sv::light_record_of(
        sv::light::point(tg::pos3f(-0.8f, -0.8f, 0.8f)).candela(lamp_power / 10.0f / (4.0f * tg::pi<f32>))));

    auto mixed = base;
    mixed.lights = sv::pt_light_table::grouped(records, sv::light_selection::power);
    auto uniform = base;
    uniform.lights = sv::pt_light_table::grouped(records, sv::light_selection::uniform);
    auto pure = mixed;
    {
        // Points stand before areas in the table; by power the corner light is one part in 11.
        REQUIRE(pure.lights.records.size() == 2);
        REQUIRE(pure.lights.records[0].path == u32(sv::light_path::point));
        pure.lights.records[0].select_pdf = 1.0f / 11.0f;
        pure.lights.records[0].select_cdf = 1.0f / 11.0f;
        pure.lights.records[1].select_pdf = 10.0f / 11.0f;
        pure.lights.records[1].select_cdf = 1.0f;
    }
    REQUIRE(mixed.lights.records[0].select_pdf > 0.25f);

    constexpr int frames = 48;
    auto const by_mixed = co_await run_restir(ctx, resources, mixed, frames, {.history = false});
    auto const by_uniform = co_await run_restir(ctx, resources, uniform, frames, {.history = false});
    auto const by_pure = co_await run_restir(ctx, resources, pure, frames, {.history = false});
    auto const by_lamp = co_await run_restir(ctx, resources, base, frames, {.history = false});

    auto const uniform_mean = mean_luminance_of(by_uniform.mean);
    auto const mixed_mean = mean_luminance_of(by_mixed.mean);
    CHECK(tg::abs(mixed_mean - uniform_mean) < 0.03f * uniform_mean)
        .context(cc::format("mixed: {} against {} uniform", mixed_mean, uniform_mean));

    // Against the three runs' mean, whose seeds they share, as the power test measures.
    auto reference = cc::vector<tg::vec3f>();
    for (auto i = isize(0); i < by_uniform.mean.size(); ++i)
        reference.push_back((by_uniform.mean[i] + by_mixed.mean[i] + by_pure.mean[i]) / 3.0f);
    // The corner: pixels where the corner light carries more than half of the reference, skipped everywhere else.
    auto outside_corner = cc::vector<i32>();
    auto corner_pixels = 0;
    for (auto i = isize(0); i < reference.size(); ++i)
    {
        auto const total = luminance_of(reference[i]);
        auto const from_lamp = luminance_of(by_lamp.mean[i]);
        auto const corner = total > 0.0f && total - from_lamp > 0.5f * total;
        outside_corner.push_back(corner ? 0 : 1);
        corner_pixels += corner ? 1 : 0;
    }
    REQUIRE(corner_pixels >= 8);

    auto uniform_error = 0.0f;
    auto mixed_error = 0.0f;
    auto pure_error = 0.0f;
    auto mixed_whole = 0.0f;
    auto pure_whole = 0.0f;
    for (auto f = 0; f < frames; ++f)
    {
        uniform_error += rms_error(by_uniform.frames[f], reference, outside_corner) / f32(frames);
        mixed_error += rms_error(by_mixed.frames[f], reference, outside_corner) / f32(frames);
        pure_error += rms_error(by_pure.frames[f], reference, outside_corner) / f32(frames);
        mixed_whole += rms_error(by_mixed.frames[f], reference) / f32(frames);
        pure_whole += rms_error(by_pure.frames[f], reference) / f32(frames);
    }
    // A pixel the corner light carries has an error that goes with one over the root of that light's pick probability:
    // 1/11 by power alone, 0.5 uniform, and about 0.295 mixed.
    // So the mixture sits near 0.55 of power alone and 1.3 of uniform, each bound with some room over it.
    CHECK(mixed_error < 0.7f * pure_error)
        .context(cc::format("corner per-frame error: {} mixed, {} by power alone", mixed_error, pure_error));
    CHECK(mixed_error < 1.5f * uniform_error)
        .context(cc::format("corner per-frame error: {} mixed, {} uniform", mixed_error, uniform_error));
    CC_LOG_INFO(
        "a dim corner light over {} pixels: per-frame error {} mixed, {} uniform, {} by power alone; whole image {} "
        "mixed, {} by power alone; means {} mixed, {} uniform",
        corner_pixels, mixed_error, uniform_error, pure_error, mixed_whole, pure_whole, mixed_mean, uniform_mean);

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR's DI track, phase D1: the visible point's light from the trace's lights, resampled and reused, on 22 small lights.
// With one next-event sample a frame a pixel sees one lamp of 22.
// Resampling eight candidates, and the neighbors' and the last frame's choices, makes the one that matters to it close to certain.
// The visibility is traced once, at the re-shade, so the estimate stays unbiased: its mean is the plain path tracer's.
// Its noise is measured against the plain run by default, and against a reference four times as long under --thorough.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the image of many lights and cuts its noise",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    auto scene = make_scene(resources);

    // 22 small lamps under the ceiling, a grid of 5 by 4 and two more, each as bright as the box's own lamp per area.
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    auto const half = 0.06f;
    auto const add_lamp = [&](f32 x, f32 z)
    {
        records.push_back({.position = tg::vec3f(x, 0.99f, z),
                           .path = u32(sv::light_path::area),
                           .u = tg::vec3f(half, 0, 0),
                           .area = 4.0f * half * half,
                           .v = tg::vec3f(0, 0, half),
                           .emission = scene.box.light.emission,
                           .normal = tg::vec3f(0, -1, 0),
                           .cone_offset = 1,
                           .link_mask = ~0u});
    };
    for (auto ix = 0; ix < 5; ++ix)
        for (auto iz = 0; iz < 4; ++iz)
            add_lamp(-0.8f + 0.4f * f32(ix), -0.6f + 0.4f * f32(iz));
    add_lamp(-0.9f, 0.9f);
    add_lamp(0.9f, -0.9f);
    scene.lights = sv::pt_light_table::grouped(records);

    constexpr int frames = 48;
    constexpr int warm = 16;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const di = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true, .di = true});

    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const di_mean = mean_luminance_of(di.mean);
    CHECK(tg::abs(di_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("ReSTIR DI: {} against {} without", di_mean, plain_mean));
    CC_LOG_INFO("D1 on the box with 22 lamps: means {} with ReSTIR DI, {} without", di_mean, plain_mean);

    // DI's own gain, over the GI reuse it runs beside.
    // Against the plain run's mean by default, whose seeds the other runs share; under --thorough against a reference four times as long on seeds of its own.
    auto const gi = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true});
    auto reference = plain.mean;
    if (nx::is_thorough())
        reference = (co_await run_restir(ctx, resources, scene, 4 * frames, {.history = false, .seeds = 1})).mean;
    auto plain_error = 0.0f;
    auto gi_error = 0.0f;
    auto di_error = 0.0f;
    for (auto f = warm; f < frames; ++f)
    {
        plain_error += rms_error(plain.frames[f], reference) / f32(frames - warm);
        gi_error += rms_error(gi.frames[f], reference) / f32(frames - warm);
        di_error += rms_error(di.frames[f], reference) / f32(frames - warm);
    }
    // The continuation's hits on the lamps keep their half of the weight, and their noise, so the gain is bounded below what DI alone would give.
    CHECK(di_error * 1.15f < gi_error)
        .context(cc::format("per-frame error: {} with ReSTIR DI, {} with GI reuse alone", di_error, gi_error));
    CC_LOG_INFO("D1 per-frame error against a reference of {}: {} with DI, {} with GI reuse alone, {} plain",
                mean_luminance_of(reference), di_error, gi_error, plain_error);

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR's re-shade lights a surface only from its face's own side, as next-event estimation does.
//
// The floor's authored normal leans 55 degrees off its face, so a light below the floor still lies above the shading frame.
// The light casts no shadow, so nothing but that rule keeps it off the side the camera sees; the same light mirrored above
// the floor shows the setup lights at all.
// A parallel light reaches the floor through next-event estimation alone, which DI resamples; a rect off toward -x through
// the continuation too, which GI resamples.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR's re-shade does not light a surface from behind its face",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f), .roughness = 0.5f};
    auto ground = sv_test::cornell_box{};
    sv_test::cb_push_quad(ground, tg::pos3f(-5, 0, -5), tg::pos3f(-5, 0, 5), tg::pos3f(5, 0, 5), tg::pos3f(5, 0, -5),
                          white);
    auto mesh = sv_test::as_mesh("tilted floor", ground.positions, ground.materials);

    // The authored normal leans off the floor's +y toward -x, so it faces both lights and the camera.
    // The frame is +z turned a quarter turn about cross(+z, authored), which carries +z onto it.
    auto const authored = tg::normalize(tg::vec3f(-1.0f, 0.7f, 0.0f));
    auto const half = tg::sqrt(0.5f);
    auto const frame = tg::vec4f(-authored[1] * half, authored[0] * half, 0.0f, half);
    mesh.attributes.push_back(
        sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex,
                                   cc::vector<tg::vec4f>::create_filled(ground.positions.size(), frame)));

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = cornell_scene{};
    scene.item = resources.acquire_scene_item(mesh);
    resources.wait_for_pending_uploads();
    REQUIRE(resources.meshes.get_ptr(scene.item.mesh) != nullptr);
    auto const* const permutation = resources.shaders.find(scene.item.shader_key);
    REQUIRE(permutation != nullptr);
    scene.instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, scene.item.mesh), .instance_id = 0, .hit_group_offset = 0});
    scene.hit_groups.push_back(permutation);
    scene.camera = sv::camera::looking_at(tg::pos3d(0, 4, -0.5), tg::pos3d(0, 0, 0));
    scene.camera.projection.aspect_ratio = 1.0;

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
    constexpr int frames = 8;
    auto const reuse = restir_reuse{.history = true, .spatial = true, .di = true, .max_bounces = 2};
    for (auto const& pair : pairs)
    {
        auto const one = [](sv::light const& l)
        {
            auto records = cc::vector<sv::shaders::tracer::light_record>();
            records.push_back(sv::light_record_of(l));
            return sv::pt_light_table::grouped(records);
        };
        scene.lights = one(pair.above);
        auto const lit = mean_luminance_of((co_await run_restir(ctx, resources, scene, frames, reuse)).mean);
        scene.lights = one(pair.below);
        auto const behind = mean_luminance_of((co_await run_restir(ctx, resources, scene, frames, reuse)).mean);
        CHECK(lit > 0.1f).context(cc::format("lit from above: {}", lit));
        CHECK(behind < 0.01f * lit).context(cc::format("lit from behind: {}, from above: {}", behind, lit));
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR DI on every kind of light at once — a rect, a two-sided rect, a point with a cone, a sun and a sky disc — over a glossy wall.
// A light reaching the visible point best through the BSDF's narrow lobe must keep its light, so the resampled mean is the plain one.
// So must the image without the spatial pass.
// With one bounce no continuation weighs against the light sample, and that runs on a matte box.
// A glossy wall's reflection of a lamp then comes from light samples alone, whose fireflies move a 48-frame mean by several percent with any change to the stream.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the mean of every kind of light over a glossy wall",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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

    auto box = sv_test::make_cornell_box();
    auto const glossy = sv_test::pbr_material{.base_color = tg::vec3f(0.9f, 0.9f, 0.9f),
                                              .metallic = 1.0f,
                                              .roughness = 0.1f,
                                              .emissive = tg::vec3f(0, 0, 0)};
    box.materials[4] = glossy;
    box.materials[5] = glossy;
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources, cc::move(box));

    auto records = cc::vector<sv::shaders::tracer::light_record>();
    records.push_back(scene.lights.records[0]);
    records.push_back({.position = tg::vec3f(0.6f, -0.2f, 0.3f),
                       .path = u32(sv::light_path::area),
                       .u = tg::vec3f(0.08f, 0, 0),
                       .area = 4.0f * 0.08f * 0.08f,
                       .v = tg::vec3f(0, 0.08f, 0),
                       .flags = sv::light_flag_two_sided,
                       .emission = tg::vec3f(6, 4, 2),
                       .normal = tg::vec3f(0, 0, 1),
                       .cone_offset = 1,
                       .link_mask = ~0u});
    records.push_back({.position = tg::vec3f(-0.5f, 0.2f, 0.0f),
                       .path = u32(sv::light_path::point),
                       .emission = tg::vec3f(0.5f, 0.5f, 0.8f),
                       .cone_scale = 2.0f,
                       .normal = tg::vec3f(0, -1, 0),
                       .cone_offset = -0.5f,
                       .link_mask = ~0u});
    records.push_back({.path = u32(sv::light_path::distant_point),
                       .emission = tg::vec3f(0.3f, 0.3f, 0.3f),
                       .normal = tg::normalize(tg::vec3f(0.2f, -1, 0.6f)),
                       .cone_offset = 1,
                       .link_mask = ~0u});
    records.push_back({.path = u32(sv::light_path::distant_disc),
                       .emission = tg::vec3f(20, 20, 20),
                       .normal = tg::normalize(tg::vec3f(-0.1f, -1, 0.8f)),
                       .cone_offset = 1,
                       .one_minus_cos_angular_radius = 0.01f,
                       .link_mask = ~0u});
    scene.lights = sv::pt_light_table::grouped(records);

    constexpr int frames = 48;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const di = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true, .di = true});
    auto const temporal = co_await run_restir(ctx, resources, scene, frames, {.history = true, .di = true});
    auto matte = make_scene(resources);
    matte.lights = scene.lights;
    auto const plain_once = co_await run_restir(ctx, resources, matte, frames, {.history = false, .max_bounces = 1});
    auto const di_once = co_await run_restir(ctx, resources, matte, frames,
                                             {.history = true, .spatial = true, .di = true, .max_bounces = 1});
    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const di_mean = mean_luminance_of(di.mean);
    auto const temporal_mean = mean_luminance_of(temporal.mean);
    auto const plain_once_mean = mean_luminance_of(plain_once.mean);
    auto const di_once_mean = mean_luminance_of(di_once.mean);
    CHECK(tg::abs(di_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("every kind of light: {} with ReSTIR DI, {} without", di_mean, plain_mean));
    CHECK(tg::abs(temporal_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("without the spatial pass: {} with ReSTIR DI, {} without", temporal_mean, plain_mean));
    CHECK(tg::abs(di_once_mean - plain_once_mean) < 0.03f * plain_once_mean)
        .context(cc::format("one bounce: {} with ReSTIR DI, {} without", di_once_mean, plain_once_mean));
    CC_LOG_INFO("D1 with every kind of light over a glossy wall: means {} with ReSTIR DI, {} without", di_mean,
                plain_mean);

    co_await cc::async_settled(sv::background_work(ctx));
}

// The furnace through the wavefront: a lossless object under a uniform environment returns the environment, which is what
// segment's medium walk and shade's refraction have to get right between them — see volumetric-furnace-test.cc for the megakernel's.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - a lossless interior is invisible under a uniform environment",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
        /// a lossy case's lower bound, where it has one, which then replaces the absorption checks
        f32 at_least = 0.0f;
        /// whether a default run traces it, or only a thorough one
        bool by_default = true;
    };
    auto cases = cc::vector<furnace_case>();
    // A default run keeps refraction, a medium walk and absorption; the clear interior and the rough glass only a thorough one.
    cases.push_back({.name = "clear interior", .bindings = base(), .by_default = false});
    {
        // Rough glass sends the sky through transmitted directions next-event estimation never samples, so they weigh 1.
        // Weighed against next-event estimation it measures ~0.386; the bound sits between that and the ~0.420 it measures now.
        // The rest of the loss is the rough transmission's uncompensated single-scattering GGX.
        // Once that is compensated, this case is lossless and belongs with the others.
        auto rough = cc::vector<binding>();
        rough.push_back(binding::of("specular_ior", 1.5f));
        rough.push_back(binding::of("specular_roughness", 0.5f));
        rough.push_back(binding::of("transmission_weight", 1.0f));
        cases.push_back({.name = "rough glass interior",
                         .bindings = cc::move(rough),
                         .lossless = false,
                         .at_least = 0.81f * luminance_of(environment),
                         .by_default = false});
    }
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
        if (!c.by_default && !nx::is_thorough())
            continue;
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
        else if (c.at_least > 0.0f)
            CHECK(mean > c.at_least).context(c.name).dump("mean luminance", mean);
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
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
    // 68 MB of rgba32_float is past the inline readback ring, which sg reports once.
    nx::expect_warning("an inline readback of * did not fit the *-byte readback ring*", nx::exactly(1));

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

    // A hazard is a property of one restart, carry and drain, so a default run traces each once.
    // A thorough run traces the furnace's full length, which its mean is pinned against.
    auto const thorough = nx::is_thorough();
    auto const frames = thorough ? 24 : 3;
    auto const rounds = thorough ? 12 : 4;

    auto resources = sv::gpu_resource_manager::create(ctx);
    {
        auto const cube = sv::mesh{.name = "vulkan wavefront cube",
                                   .geometry = sv::triangle_geometry::create_from_positions(closed_cube(0.6f)),
                                   .material = lib.acquire(sv::material::create("vulkan wavefront", type, bindings))};
        auto const pixels = co_await trace_wavefront(ctx, resources, cube,
                                                     {.camera = camera,
                                                      .environment = sv::background::uniform(environment),
                                                      .max_bounces = 12,
                                                      .frames = frames,
                                                      .rounds = rounds,
                                                      .drain_rounds = thorough ? 96 : 16});
        auto mean = 0.0f;
        for (auto const& p : pixels)
            mean += luminance_of(p);
        mean /= f32(pixels.size());
        CHECK(tg::is_finite(mean)).dump("mean luminance", mean);
        if (thorough)
        {
            CHECK(tg::abs(mean - luminance_of(environment)) <= 0.06f * luminance_of(environment)).dump("mean luminance", mean);
        }
    }
    {
        auto const box = sv_test::make_cornell_box();
        auto box_camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
        box_camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
        auto const pixels = co_await trace_wavefront(
            ctx, resources, sv_test::as_mesh("vulkan wavefront box", box.positions, box.materials),
            {.camera = box_camera,
             .environment = sv::background::uniform(environment),
             .frames = frames,
             .rounds = rounds,
             .drain_rounds = thorough ? 48 : drain_rounds_of(k_max_bounces)});
        auto finite = true;
        for (auto const& p : pixels)
            finite = finite && tg::is_finite(p[0]) && tg::is_finite(p[1]) && tg::is_finite(p[2]);
        CHECK(finite);
    }

    co_await cc::async_settled(sv::background_work(ctx));
    CHECK(!ctx.is_device_lost());
}
#endif
