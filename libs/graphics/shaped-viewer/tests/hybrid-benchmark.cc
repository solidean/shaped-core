#include "viewer_test_env.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/impl/gpu_marks.hh> // TEMPORARY(debt-sv-gpu-stage-marks)

using namespace cc::primitive_defines;

// What a whole moving frame costs on Intel's Sponza, through the viewer: the hybrid, its denoiser and its upscaler, against the 16.6 ms of 60 fps.
//
// Every frame moves the camera along the nave, so every frame is one a user orbiting sees: drawn at the upscaler's input extent and upscaled to the output.
// Each contender runs alone, its view and history freed before the next: four views' histories and path state at once overflow a laptop GPU's memory.
// GPU time, bracketed by timestamps around the viewer's whole frame, each frame alone on the GPU.
// A benchmark rather than a test: it asserts nothing, and libs/graphics/shaped-viewer/docs/plans/hybrid-renderer.md records what it measured.

namespace
{
constexpr auto k_output = tg::vec2i(1920, 1200);
constexpr int k_frames = 48;

/// One way of drawing the frame, as a view's layer settings say it.
struct contender
{
    cc::string_view name;
    sv::render_settings settings;
};

/// A definition holding one view of `items` drawn under `settings`, filling the output.
[[nodiscard]] sv::viewer_definition definition_of(sv::view_id id,
                                                  cc::span<sv::scene_item const> items,
                                                  sv::render_settings const& settings)
{
    auto def = sv::viewer_definition{};
    auto v = sv::view_data{};
    v.id = id;
    auto& scene = sv::ensure_scene_3d(v);
    for (auto const& item : items)
        scene.items.push_back(item);
    scene.settings = settings;
    scene.background = sv::background::studio();
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
} // namespace

ASYNC_BENCHMARK("sv - the hybrid's whole moving frame on Sponza, denoised and upscaled to 1920x1200")
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the shaders");
    auto& ctx = *created.value();
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("no ray tracing on this device");
    CC_LOG_INFO("adapter: {}", ctx.metrics.adapter().name);

    // What this process holds on the GPU against what it may, at each step: the differences are what each step costs.
    auto const memory = [&](cc::string_view when)
    {
        auto const usage = ctx.metrics.query_gpu_memory();
        if (usage.has_value())
            CC_LOG_INFO("gpu memory {}: {} MiB of a {} MiB budget", when, usage.value().current_usage_bytes >> 20,
                        usage.value().budget_bytes >> 20);
    };
    memory("with the context alone");

    auto const loader = sv::asset_loader();
    auto const asset = loader.load(SV_TEST_SPONZA_DIR "/NewSponza_Main_glTF_003.gltf");
    if (asset.has_error())
        SKIP(cc::format("Sponza did not load ({}); uv run extern/sponza/fetch-sponza.py installs it",
                        asset.error().to_string()));
    auto const box = asset.value().bounds();
    REQUIRE(box.has_value());

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto items = cc::vector<sv::scene_item>();
    for (auto const& mesh : asset.value().meshes)
        items.push_back(resources.acquire_scene_item(mesh));
    resources.wait_for_pending_uploads();
    memory("with Sponza uploaded");

    auto const support = sr::query_reconstruct_support(ctx);
    auto const dlss
        = sr::reconstruct_settings{.denoiser = sr::denoise_method::dlss_rr, .scale = sr::render_scale_preset::quality};
    auto const fsr = sr::reconstruct_settings{.denoiser = sr::denoise_method::svgf,
                                              .upscaler = sr::upscale_method::fsr,
                                              .scale = sr::render_scale_preset::quality};
    auto contenders = cc::vector<contender>();
    if (support.dlss_rr)
    {
        contenders.push_back({.name = "hybrid deferred, dlss_rr",
                              .settings = {.mode = sv::render_mode::hybrid,
                                           .hybrid_lighting = sv::hybrid_lighting::deferred,
                                           .reconstruct = dlss}});
        contenders.push_back(
            {.name = "hybrid forward, dlss_rr",
             .settings
             = {.mode = sv::render_mode::hybrid, .hybrid_lighting = sv::hybrid_lighting::forward, .reconstruct = dlss}});
        contenders.push_back(
            {.name = "wavefront, dlss_rr", .settings = {.mode = sv::render_mode::wavefront, .reconstruct = dlss}});
    }
    if (support.fsr && support.svgf)
        contenders.push_back(
            {.name = "hybrid deferred, svgf + fsr",
             .settings
             = {.mode = sv::render_mode::hybrid, .hybrid_lighting = sv::hybrid_lighting::deferred, .reconstruct = fsr}});
    if (contenders.empty())
        SKIP("neither dlss_rr nor svgf with fsr runs on this device");

    // At head height, walking along the box's longer horizontal axis, as the raster benchmark walks it.
    auto const lo = box.value().min;
    auto const hi = box.value().max;
    auto const along_x = hi[0] - lo[0] >= hi[2] - lo[2];
    auto const camera_at = [&](int frame)
    {
        auto const t = 0.15 + 0.7 * f64(frame) / f64(k_frames);
        auto const center = tg::pos3d(0.5 * (f64(lo[0]) + f64(hi[0])), f64(lo[1]) + 1.7, 0.5 * (f64(lo[2]) + f64(hi[2])));
        auto const position = along_x ? tg::pos3d(f64(lo[0]) + t * f64(hi[0] - lo[0]), center[1], center[2])
                                      : tg::pos3d(center[0], center[1], f64(lo[2]) + t * f64(hi[2] - lo[2]));
        auto const ahead = along_x ? tg::vec3d(1, 0, 0) : tg::vec3d(0, 0, 1);
        auto cam = sv::camera::looking_at(position, position + ahead);
        cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
        return cam;
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
        auto def = definition_of(id, items, c.settings);
        auto store = sv::view_store{};
        auto const execute = [&](sg::command_list& cmd, int frame)
        {
            def.views[0].camera = camera_at(frame);
            resources.advance_to(ctx.current_epoch());
            store.begin_frame(u64(ctx.current_epoch()));
            auto const plan = sv::build_render_plan(def, k_output, u64(frame), history);
            return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                                output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        };
        auto const accumulated = [&]
        {
            auto const* const rec = store.peek_ptr(id);
            auto const* const slot = rec != nullptr ? rec->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
            return slot != nullptr ? slot->accum_frame : u32(0);
        };

        // Held still until the mean climbs, which is when every permutation and member has compiled.
        auto const start = cc::current_time_steady_secs();
        while (true)
        {
            (void)ctx.routines.tick();
            auto cmd = ctx.create_command_list();
            (void)resources.record_pending_work(*cmd);
            (void)execute(*cmd, 0);
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            if (accumulated() >= 2 && resources.pending_work_count() == 0)
                break;
            REQUIRE(cc::current_time_steady_secs() - start < 600.0);
            sv_test::drive_ambient_work();
        }

        memory(cc::format("once {} is warm", c.name));

        // Throughput first: frames back to back, as a viewer draws them, over the wall clock from the first submit until the GPU idles.
        // Their own timestamps would overstate them, since a member's work may run on past its frame and into the next one's stages.
        auto const wall_start = cc::current_time_steady_secs();
        for (auto f = 1; f <= k_frames; ++f)
        {
            auto cmd = ctx.create_command_list();
            auto const outcome = execute(*cmd, f);
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            REQUIRE(outcome == sg::routine_outcome::executed);
        }
        co_await ctx.idle_completion();
        auto const wall = cc::current_time_steady_secs() - wall_start;

        // Each frame's stages, as the marks the viewer records along it divide it: each mark ends the stage it names.
        auto frames = cc::vector<sv::impl::gpu_marks>();
        for (auto f = 1; f <= k_frames; ++f)
        {
            frames.push_back({});
            sv::impl::active_gpu_marks = &frames.back();
            auto cmd = ctx.create_command_list();
            sv::impl::gpu_mark(*cmd, "start");
            auto const outcome = execute(*cmd, f);
            sv::impl::gpu_mark(*cmd, "the rest of the frame");
            sv::impl::active_gpu_marks = nullptr;
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            REQUIRE(outcome == sg::routine_outcome::executed);
            // Each frame alone on the GPU, so no work of the frame before it lands inside this one's stages.
            co_await ctx.idle_completion();
        }

        // Per stage, in the order the first frame met them, the mean over the frames that recorded it.
        struct stage
        {
            cc::string_view name;
            f64 seconds = 0.0;
        };
        auto stages = cc::vector<stage>();
        auto total = 0.0;
        for (auto const& fr : frames)
        {
            total += fr.marks.back().at.try_get_seconds().value() - fr.marks.front().at.try_get_seconds().value();
            for (auto i = isize(1); i < fr.marks.size(); ++i)
            {
                auto const dt = fr.marks[i].at.try_get_seconds().value() - fr.marks[i - 1].at.try_get_seconds().value();
                auto found = false;
                for (auto& st : stages)
                    if (st.name == fr.marks[i].stage)
                    {
                        st.seconds += dt;
                        found = true;
                    }
                if (!found)
                    stages.push_back({.name = fr.marks[i].stage, .seconds = dt});
            }
        }
        memory(cc::format("after {}'s frames", c.name));
        auto const* const samples = store.peek_ptr(id)->temporal.get_ptr(sv::temporal_id::frame_samples(0));
        auto const input = samples != nullptr ? samples->scaled_denoise.extent() : tg::vec2i(0, 0);
        CC_LOG_INFO("sponza {}x{} moving: {} {} ms per frame alone on the GPU, {} ms back to back, drawn at {}x{}",
                    k_output[0], k_output[1], c.name, 1e3 * total / f64(k_frames), 1e3 * wall / f64(k_frames), input[0],
                    input[1]);
        for (auto const& st : stages)
            CC_LOG_INFO("    {}: {} ms", st.name, 1e3 * st.seconds / f64(k_frames));
    }

    co_await cc::async_settled(sv::background_work(ctx));
    co_await ctx.idle_completion();
    co_return;
}
