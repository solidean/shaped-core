#include "viewer_test_env.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>

using namespace cc::primitive_defines;

// The viewer's tracers through SGL's own binary against the same SGL's text through DXC, in GPU time per frame.
//
// Two contexts of one backend on one device, one per route, so every shader the tracers build, hit groups and wavefront
// kernels included, comes through one route or the other.
// Their frames alternate one by one, so a GPU whose clocks drift under load treats both alike.
// A benchmark rather than a test: it asserts nothing, and libs/graphics/shaped-graphics-language/docs/native-parity.md
// records what it measured.

namespace
{
constexpr auto k_size = tg::vec2i(512, 512);
constexpr int k_frames = 24;
constexpr i32 k_spp = 4;
constexpr int k_rounds = 8;

/// The GPU seconds between each pair of timestamps, once all of them have landed.
[[nodiscard]] f64 seconds_of(cc::span<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp> const> spans)
{
    auto seconds = 0.0;
    for (auto const& [from, to] : spans)
        seconds += to.try_get_seconds().value() - from.try_get_seconds().value();
    return seconds;
}

/// The cornell box of the wavefront benchmark: a shallow scene, lit by an area light, whose paths reach every stage.
struct scene_rig
{
    sg::context* ctx = nullptr;
    sv::gpu_resource_manager resources;
    sg::texture_2d target;
    sv::wavefront_state state;
    sv::scene_item item;
    cc::vector<sg::tlas_instance> instances;
    cc::vector<sv::material_permutation const*> hit_groups;
    sv::pt_light_table lights;
    sv::background_gpu background;
    sv::camera camera;
    cc::optional<sv_test::area_light> light;

    explicit scene_rig(sg::context& c)
      : ctx(&c),
        resources(sv::gpu_resource_manager::create(c)),
        target(c.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                               .width = k_size[0],
                                               .height = k_size[1],
                                               .usage = sg::texture_usage::texture | sg::texture_usage::image})),
        state(sv::wavefront_state::create(c, k_size, 4))
    {
        auto const box = sv_test::make_cornell_box();
        camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
        camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
        light = box.light;
        auto const mesh = sv_test::as_mesh("native route cornell", box.positions, box.materials);
        item = resources.acquire_scene_item(mesh);
        resources.wait_for_pending_uploads();
        auto const* const permutation = resources.shaders.find(item.shader_key);
        CC_ASSERT(permutation != nullptr, "the cornell box's material must have a permutation");
        instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh),
                                              .instance_id = 0,
                                              .hit_group_offset = 0,
                                              .opaque_override = !permutation->opacity.can_cut_out()});
        hit_groups.push_back(permutation);
        lights = sv_test::light_table_of(light.value());
        background = sv::background_gpu::from(sv::background());
    }

    [[nodiscard]] sv::shaders::tracer::frame_constants frame_of(u32 seed, u32 accum_frame, i32 spp) const
    {
        auto fc = sv::default_frame_constants();
        fc.camera = sv::camera_record_of(camera);
        fc.previous_camera = fc.camera;
        lights.describe_in(fc);
        fc.samples_per_pixel = spp;
        fc.max_bounces = 5;
        fc.rng_seed = seed;
        fc.accum_frame = accum_frame;
        return fc;
    }

    [[nodiscard]] sv::pt_trace_desc trace_of(sg::command_list& cmd,
                                             sg::buffer<sv::shaders::tracer::frame_constants> const& frame,
                                             sv::bound_resources const& bindless)
    {
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[0], hit_groups[0]->opacity);
        return sv::pt_trace_desc{
            .frame = frame,
            .background = ctx->transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
            .instances = instances,
            .output = target,
            .instance_table = ctx->transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
            .lights = sv_test::upload_lights(cmd, lights),
            .hit_groups = hit_groups,
            .bindless = &bindless};
    }

    /// One frame of the megakernel (`wavefront` false) or of the wavefront at `k_rounds`, on this rig's context.
    sg::routine_outcome record(sg::command_list& cmd, bool wavefront, int f, sv::wavefront_timing* timing = nullptr)
    {
        auto const bindless = resources.freeze();
        if (!wavefront)
        {
            auto const fc = frame_of(u32(f) + 1u, u32(f), k_spp);
            auto const frame = ctx->transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);
            return sv::pathtrace_routine::execute(cmd, trace_of(cmd, frame, bindless));
        }
        auto const fc = frame_of(7u, 0u, 1);
        auto const frame = ctx->transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);
        return sv::wavefront_routine::execute(cmd, {.trace = trace_of(cmd, frame, bindless),
                                                    .frame = fc,
                                                    .state = &state,
                                                    .rounds = k_rounds,
                                                    .restart = f == 0,
                                                    .regenerate = true,
                                                    .roulette_after = 3,
                                                    .timing = timing});
    }
};

/// Both tracers on both routes, each frame of one route followed by the same frame of the other.
cc::shared_async<cc::unit> compare_routes(sg::context& text, sg::context& native, cc::string_view backend)
{
    for (auto* const ctx : {&text, &native})
    {
        ctx->download.set_budget(isize(1) << 24);
        ctx->advance_epoch();
    }
    auto rigs = cc::vector<std::unique_ptr<scene_rig>>();
    rigs.push_back(std::make_unique<scene_rig>(text));
    rigs.push_back(std::make_unique<scene_rig>(native));

    for (auto const wavefront : {false, true})
    {
        // Untimed until each route has executed once, which is what its shaders compiling takes.
        auto const start = cc::current_time_steady_secs();
        for (auto& rig : rigs)
        {
            auto ready = false;
            while (!ready)
            {
                (void)rig->ctx->routines.tick();
                auto cmd = rig->ctx->create_command_list();
                ready = rig->record(*cmd, wavefront, 0) == sg::routine_outcome::executed;
                rig->ctx->submit_command_list(cc::move(cmd));
                rig->ctx->advance_epoch();
                co_await rig->ctx->idle_completion();
                REQUIRE(cc::current_time_steady_secs() - start < 170.0);
                if (!ready)
                    sv_test::drive_ambient_work();
            }
        }

        // A material's shade kernel compiles in the background, and until it lands the wavefront shades with a
        // stand-in, so a frame is timed only once every compile a frame starts has settled.
        for (auto settle = 0; settle < 3; ++settle)
            for (auto& rig : rigs)
            {
                auto cmd = rig->ctx->create_command_list();
                (void)rig->record(*cmd, wavefront, 1);
                rig->ctx->submit_command_list(cc::move(cmd));
                rig->ctx->advance_epoch();
                co_await rig->ctx->idle_completion();
                co_await cc::async_settled(sv::background_work(*rig->ctx));
            }

        auto spans = cc::vector<cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>>();
        spans.push_back({});
        spans.push_back({});
        auto timings = cc::vector<cc::vector<sv::wavefront_timing>>();
        for (auto r = 0; r < 2; ++r)
            timings.push_back(cc::vector<sv::wavefront_timing>::create_defaulted(k_frames));
        // a warm-up of a few frames each, untimed, past the driver's first tier of a new pipeline
        for (auto f = -4; f < k_frames; ++f)
            for (auto r = isize(0); r < rigs.size(); ++r)
            {
                auto& rig = *rigs[r];
                auto cmd = rig.ctx->create_command_list();
                auto const from = cmd->query.record_gpu_timestamp();
                auto* const timing = wavefront && f >= 0 ? &timings[r][f] : nullptr;
                auto const outcome = rig.record(*cmd, wavefront, cc::max(f, 0) + 1, timing);
                auto const to = cmd->query.record_gpu_timestamp();
                rig.ctx->submit_command_list(cc::move(cmd));
                rig.ctx->advance_epoch();
                co_await rig.ctx->idle_completion();
                REQUIRE(outcome == sg::routine_outcome::executed);
                if (f >= 0)
                    spans[r].push_back({from, to});
            }
        auto const text_ms = 1e3 * seconds_of(spans[0]) / k_frames;
        auto const native_ms = 1e3 * seconds_of(spans[1]) / k_frames;
        CC_LOG_INFO("{} {}: text {} ms per frame, native {} ms per frame, {}x text", backend,
                    wavefront ? "wavefront" : "megakernel", text_ms, native_ms, native_ms / text_ms);
        if (!wavefront)
            continue;
        // which kernel of the wavefront the difference is in, summed over every frame
        auto stages = cc::vector<cc::fixed_array<f64, sv::wavefront_timing::stage_count>>();
        for (auto r = 0; r < 2; ++r)
        {
            stages.push_back({});
            for (auto const& t : timings[r])
            {
                auto const landed = t.try_seconds();
                REQUIRE(landed.has_value());
                for (auto k = isize(0); k < stages[r].size(); ++k)
                    stages[r][k] += landed.value()[k];
            }
        }
        constexpr cc::string_view names[]
            = {"begin", "extend", "segment", "sort", "shade", "connect", "finalize", "arguments", "resolve"};
        for (auto k = isize(0); k < stages[0].size(); ++k)
            CC_LOG_INFO("{} wavefront {}: text {} ms, native {} ms, {}x text", backend, names[k], 1e3 * stages[0][k],
                        1e3 * stages[1][k], stages[0][k] > 0.0 ? stages[1][k] / stages[0][k] : 0.0);
    }

    for (auto& rig : rigs)
    {
        co_await cc::async_settled(sv::background_work(*rig->ctx));
        co_await rig->ctx->idle_completion();
    }
}
} // namespace

ASYNC_BENCHMARK("sv - the tracers through SGL's own binary against its text through DXC, GPU time per frame, on dx12")
{
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");
    auto text = sg::create_dx12_context({.native_dxil = false, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    auto native = sg::create_dx12_context({.native_dxil = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (text.has_error() || native.has_error())
        SKIP("no dx12 hardware device");
    if (!text.value()->supports(sg::feature::ray_query) || !text.value()->supports(sg::feature::raytracing_pipeline))
        SKIP("the tracers need ray tracing");
    CC_LOG_INFO("adapter: {}", text.value()->metrics.adapter().name);
    co_await compare_routes(*text.value(), *native.value(), "dx12");
}

#if SV_TEST_HAS_VULKAN
ASYNC_BENCHMARK("sv - the tracers through SGL's own binary against its text through DXC, GPU time per frame, on vulkan")
{
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");
    auto text = sg::create_vulkan_context({.native_spirv = false});
    auto native = sg::create_vulkan_context({.native_spirv = true});
    if (text.has_error() || native.has_error())
        SKIP("no vulkan device");
    if (!text.value()->supports(sg::feature::ray_query) || !text.value()->supports(sg::feature::raytracing_pipeline))
        SKIP("the tracers need ray tracing");
    CC_LOG_INFO("adapter: {}", text.value()->metrics.adapter().name);
    co_await compare_routes(*text.value(), *native.value(), "vulkan");
}
#endif
