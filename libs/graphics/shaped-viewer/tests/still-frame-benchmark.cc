#include "viewer_test_env.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/record/hot_functions.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <shaped-viewer/impl/gpu_marks.hh> // TEMPORARY(debt-sv-gpu-stage-marks)

using namespace cc::primitive_defines;

// What one sample of a still image costs, as a capture accumulates it: the camera held, the mean climbing toward a sample target.
//
// A capture writes a frame only once its mean reaches the target, so a 256-sample frame of a video is 256 samples' worth of viewer frames.
// Where a mode traces one sample a frame, every sample pays the frame's fixed costs — recording on the CPU, the TLAS, the raster — and this splits the two.
// The scene is a lit Cornell box holding `n` small placed objects, since a CAD scene's instance count is what a frame's recording grows with.
// A benchmark rather than a test: it asserts nothing.

namespace
{
constexpr auto k_output = tg::vec2i(1920, 1080);
constexpr int k_frames = 48;
constexpr int k_in_flight = 2; // what a headless capture holds in flight, its default buffer count

struct contender
{
    cc::string_view name;
    sv::render_settings settings;
    u32 samples_per_frame = 1; // what a still hybrid frame packs, as `viewer_definition::still_samples_per_frame` says
};

[[nodiscard]] cc::result<sg::context_handle> tracing_context()
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        return cc::error("no dx12 hardware device");
    if (!sv_test::shared_env().has_compiler)
        return cc::error("no SGL compiler that reaches DXIL to build the shaders");
    if (!created.value()->supports(sg::feature::ray_query)
        || !created.value()->supports(sg::feature::raytracing_pipeline))
        return cc::error("no ray tracing on this device");
    CC_LOG_INFO("adapter: {}", created.value()->metrics.adapter().name);
    return cc::move(created.value());
}

/// The box, its lamp, and `count` placements of one small object on a grid filling the box's lower half.
[[nodiscard]] cc::vector<sv::scene_item> scene_of(sv::gpu_resource_manager& resources, int count)
{
    auto const box = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 40.0f);
    auto items = cc::vector<sv::scene_item>();
    items.push_back(resources.acquire_scene_item(box.enclosure));
    items.push_back(resources.acquire_scene_item(box.lamp));

    auto const cloud = sv_test::make_triangle_cloud(64);
    auto const object = resources.acquire_scene_item(sv_test::as_mesh("object", cloud.positions, cloud.materials));
    auto side = 1;
    while (side * side * side < count)
        ++side;
    auto const step = 1.6f / f32(side);
    auto const scale = 0.12f * step;
    for (auto i = 0; i < count; ++i)
    {
        auto const x = -0.8f + (f32(i % side) + 0.5f) * step;
        auto const y = -0.95f + (f32((i / side) % side) + 0.5f) * step * 0.5f;
        auto const z = -0.8f + (f32(i / (side * side)) + 0.5f) * step;
        auto placed = object;
        placed.transform
            = tg::affine_transform3f::make_trs(tg::vec3f(x, y, z), tg::quat_f::identity, tg::vec3f(scale, scale, scale));
        items.push_back(placed);
    }
    return items;
}

[[nodiscard]] sv::viewer_definition definition_of(sv::view_id id,
                                                  cc::span<sv::scene_item const> items,
                                                  sv::render_settings const& settings,
                                                  u32 samples_per_frame)
{
    auto def = sv::viewer_definition{};
    def.still_samples_per_frame = samples_per_frame;
    // As a capture sets them: a target the mean climbs toward, and the spatial denoiser kept for it.
    def.sample_target = 1u << 20;
    def.denoise_at_target_only = true;

    auto v = sv::view_data{};
    v.id = id;
    v.camera = sv::camera::looking_at(tg::pos3d(0, 0, -3.4), tg::pos3d(0, 0, 0));
    auto& scene = sv::ensure_scene_3d(v);
    for (auto const& item : items)
        scene.items.push_back(item);
    scene.settings = settings;
    def.views.push_back(cc::move(v));

    auto const root_node = def.nodes.add_container(sv::invalid_node);
    auto leaf = sv::layout_leaf{};
    leaf.views.push_back(sv::view_index(0));
    def.nodes.add_leaf(root_node, cc::move(leaf));
    auto root = sv::view_data{};
    root.id = sv::view_id::from_string("root");
    root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
    def.root_view = sv::view_index(def.views.size());
    def.views.push_back(cc::move(root));
    return def;
}

cc::shared_async<cc::unit> measure(sg::context& ctx, int object_count)
{
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const items = scene_of(resources, object_count);
    resources.wait_for_pending_uploads();

    auto const denoised = sr::reconstruct_settings{.denoiser = sr::denoise_method::automatic};
    auto const contenders = cc::vector<contender>{
        {.name = "hybrid deferred",
         .settings
         = {.mode = sv::render_mode::hybrid, .hybrid_lighting = sv::hybrid_lighting::deferred, .reconstruct = denoised}},
        {.name = "hybrid deferred, packing 16",
         .settings
         = {.mode = sv::render_mode::hybrid, .hybrid_lighting = sv::hybrid_lighting::deferred, .reconstruct = denoised},
         .samples_per_frame = 16},
        {.name = "megakernel, 4 spp", .settings = {.mode = sv::render_mode::megakernel, .reconstruct = denoised}},
        {.name = "wavefront, 8 rounds", .settings = {.mode = sv::render_mode::wavefront, .reconstruct = denoised}},
    };

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = k_output[0],
                                                          .height = k_output[1],
                                                          .usage = sg::texture_usage::render_target});
    auto const history = sv::view_history{};
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float);

    for (auto const& c : contenders)
    {
        auto const id = sv::view_id::from_string(c.name);
        auto const def = definition_of(id, items, c.settings, c.samples_per_frame);
        auto store = sv::view_store{};
        auto frame_index = u64(0);
        auto const execute = [&](sg::command_list& cmd)
        {
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            auto const plan = sv::build_render_plan(def, k_output, frame_index++, history);
            return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                                output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        };
        auto const slot = [&]() -> sv::impl::temporal_slot const*
        {
            auto const* const rec = store.peek_ptr(id);
            return rec != nullptr ? rec->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
        };
        auto const frames = [&] { return slot() != nullptr ? slot()->accum_frame : u32(0); };
        auto const samples = [&] { return slot() != nullptr ? slot()->samples : u32(0); };

        // Past the temporal member's hand-off, where a capture of more than a few dozen samples spends nearly all its frames.
        auto const start = cc::current_time_steady_secs();
        while (frames() < 40 || resources.pending_work_count() != 0)
        {
            (void)ctx.routines.tick();
            auto cmd = ctx.create_command_list();
            (void)resources.record_pending_work(*cmd);
            (void)execute(*cmd);
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            REQUIRE(cc::current_time_steady_secs() - start < 600.0);
            sv_test::drive_ambient_work();
        }

        // Back to back, as a headless capture records them: wall clock, and the CPU's recording within it, sampled.
        auto capture = cc::rec::recording_listener();
        auto const capture_handle = cc::rec::register_listener(capture);
        cc::rec::start_sampling({.rate_hz = 2000.0});
        auto const samples_before = samples();
        auto const wall_start = cc::current_time_steady_secs();
        auto recording = 0.0;
        for (auto f = 0; f < k_frames; ++f)
        {
            co_await ctx.epochs_in_flight_completion(k_in_flight);
            auto const record_start = cc::current_time_steady_secs();
            auto cmd = ctx.create_command_list();
            auto const outcome = execute(*cmd);
            recording += cc::current_time_steady_secs() - record_start;
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            REQUIRE(outcome == sg::routine_outcome::executed);
        }
        cc::rec::stop_sampling();
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(capture_handle);
        CC_LOG_INFO("{} objects, {}: where the CPU's time went:\n{}", object_count, c.name,
                    cc::rec::hot_functions(capture.take().spliced_samples(), {.skip_idle = true}).to_string(40));
        co_await ctx.idle_completion();
        auto const wall = cc::current_time_steady_secs() - wall_start;
        // A wavefront layer's count reads back a frame or more late, so it is let land before it is read.
        for (auto i = 0; i < 4; ++i)
        {
            auto cmd = ctx.create_command_list();
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            (void)ctx.routines.tick();
        }
        auto const added = samples() - samples_before;

        // Each frame alone on the GPU, split at the stage marks the viewer records.
        struct stage
        {
            cc::string_view name;
            f64 seconds = 0.0;
        };
        auto stages = cc::vector<stage>();
        auto gpu_total = 0.0;
        for (auto f = 0; f < k_frames; ++f)
        {
            auto marks = sv::impl::gpu_marks();
            sv::impl::active_gpu_marks = &marks;
            auto cmd = ctx.create_command_list();
            sv::impl::gpu_mark(*cmd, "start");
            auto const outcome = execute(*cmd);
            sv::impl::gpu_mark(*cmd, "the rest of the frame");
            sv::impl::active_gpu_marks = nullptr;
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            REQUIRE(outcome == sg::routine_outcome::executed);
            co_await ctx.idle_completion();

            gpu_total
                += marks.marks.back().at.try_get_seconds().value() - marks.marks.front().at.try_get_seconds().value();
            for (auto i = isize(1); i < marks.marks.size(); ++i)
            {
                auto const dt
                    = marks.marks[i].at.try_get_seconds().value() - marks.marks[i - 1].at.try_get_seconds().value();
                auto found = false;
                for (auto& st : stages)
                    if (st.name == marks.marks[i].stage)
                    {
                        st.seconds += dt;
                        found = true;
                    }
                if (!found)
                    stages.push_back({.name = marks.marks[i].stage, .seconds = dt});
            }
        }

        auto const per_frame = 1e3 * wall / f64(k_frames);
        auto const samples_per_frame = f64(added) / f64(k_frames);
        CC_LOG_INFO("{} objects, {} at {}x{}, still: {} ms a frame back to back, {} ms of it recording on the CPU, "
                    "{} ms alone on the GPU; {} samples a frame, so {} ms a sample",
                    object_count, c.name, k_output[0], k_output[1], per_frame, 1e3 * recording / f64(k_frames),
                    1e3 * gpu_total / f64(k_frames), samples_per_frame,
                    samples_per_frame > 0 ? per_frame / samples_per_frame : 0.0);
        for (auto const& st : stages)
            CC_LOG_INFO("    {}: {} ms", st.name, 1e3 * st.seconds / f64(k_frames));
    }

    co_await cc::async_settled(sv::background_work(ctx));
    co_await ctx.idle_completion();
    co_return;
}
} // namespace

ASYNC_BENCHMARK("sv - a still frame's cost per sample, by mode and instance count")
{
    auto ctx = tracing_context();
    if (ctx.has_error())
        SKIP(ctx.error().to_string());
    for (auto const count : {1, 1000, 10000})
        co_await measure(*ctx.value(), count);
}
