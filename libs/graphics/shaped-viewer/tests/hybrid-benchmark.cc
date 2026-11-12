#include "viewer_test_env.hh"

#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/hot_functions.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/impl/gpu_marks.hh> // TEMPORARY(debt-sv-gpu-stage-marks)

using namespace cc::primitive_defines;

// What a whole moving frame costs on Intel's Sponza and NVIDIA's Bistro, through the viewer: the hybrid, its denoiser and its upscaler, against the 16.6 ms of 60 fps.
//
// Every frame moves the camera along the scene, so every frame is one a user orbiting sees: drawn at the upscaler's input extent and upscaled to the output.
// Each contender runs alone, its view and history freed before the next: four views' histories and path state at once overflow a laptop GPU's memory.
// GPU time, bracketed by timestamps around the viewer's whole frame, each frame alone on the GPU.
// A benchmark rather than a test: it asserts nothing, and libs/graphics/shaped-viewer/docs/plans/hybrid-renderer.md records what it measured.

namespace
{
constexpr auto k_output = tg::vec2i(1920, 1200);
constexpr int k_frames = 48;
constexpr int k_in_flight = 2;

/// One way of drawing the frame, as a view's layer settings say it.
struct contender
{
    cc::string_view name;
    sv::render_settings settings;
};

/// `captured` as a Chrome trace in the temp directory, named for the contender `name`.
void write_trace(cc::rec::recording const& captured, cc::string_view name)
{
    auto file = cc::string(name);
    for (auto const ch : {' ', ',', '+'})
        (void)file.replace_all(ch, '-');
    auto const path = cc::format("{}/sv-hybrid-cpu-{}.json", cc::temp_directory_path(), file);
    auto opened = cc::file_write_stream_adapter::create(path);
    if (opened.has_error())
        return;
    cc::write_stream stream = opened.value().stream();
    if (babel::chrome_trace::write(stream, captured).has_error() || stream.flush().has_error())
        return;
    CC_LOG_INFO("wrote {}", path);
}

/// A definition holding one view of `items` drawn under `settings`, filling the output.
/// With `lights` empty the layer keeps its fallback light, as a scene with no lights of its own would.
[[nodiscard]] sv::viewer_definition definition_of(sv::view_id id,
                                                  cc::span<sv::scene_item const> items,
                                                  cc::span<sv::scene_light const> lights,
                                                  sv::background const& background,
                                                  sv::render_settings const& settings)
{
    auto def = sv::viewer_definition{};
    auto v = sv::view_data{};
    v.id = id;
    auto& scene = sv::ensure_scene_3d(v);
    for (auto const& item : items)
        scene.items.push_back(item);
    for (auto const& l : lights)
        scene.lights.push_back(l);
    if (!lights.empty())
        scene.fallback_light = cc::nullopt;
    scene.settings = settings;
    scene.background = background;
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

/// A dx12 hardware context that traces and builds the shaders, or none, with why, where this machine has no such context.
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

/// Times contender `c` drawing `items` into a frame of `output`, with `camera_at` the camera of each frame, and logs it.
/// Back to back as a viewer draws, with the CPU's share sampled; then each frame alone on the GPU, divided into the stages the viewer marks.
/// Awaited on the spot, so what it takes by reference outlives every suspend.
cc::shared_async<cc::unit> time_contender(sg::context& ctx,
                                          sv::gpu_resource_manager& resources,
                                          cc::string_view name,
                                          contender const& c,
                                          cc::span<sv::scene_item const> items,
                                          cc::span<sv::scene_light const> lights,
                                          sv::background const& background,
                                          tg::vec2i output_size,
                                          cc::function_ref<sv::camera(int)> camera_at)
{
    auto const memory = [&](cc::string_view when)
    {
        auto const usage = ctx.metrics.query_gpu_memory();
        if (usage.has_value())
            CC_LOG_INFO("gpu memory {}: {} MiB of a {} MiB budget", when, usage.value().current_usage_bytes >> 20,
                        usage.value().budget_bytes >> 20);
    };
    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto const history = sv::view_history{};
    auto const id = sv::view_id::from_string(c.name);
    auto def = definition_of(id, items, lights, background, c.settings);
    auto store = sv::view_store{};
    auto const execute = [&](sg::command_list& cmd, int frame)
    {
        def.views[0].camera = camera_at(frame);
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, u64(frame), history);
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
    // At most k_in_flight epochs in flight, as a swapchain of that many images holds a windowed viewer to.
    // Their own timestamps would overstate them, since a member's work may run on past its frame and into the next one's stages.
    // The CPU's share is split in two: recording the frame, and handing it over, which is where any wait on the GPU would show.
    // The recording is sampled too, and where its time went is logged and written out as a Chrome trace.
    auto capture = cc::rec::recording_listener();
    auto const capture_handle = cc::rec::register_listener(capture);
    cc::rec::start_sampling({.rate_hz = 2000.0});
    auto const wall_start = cc::current_time_steady_secs();
    auto recording = 0.0;
    auto handing_over = 0.0;
    auto throttled = 0.0;
    for (auto f = 1; f <= k_frames; ++f)
    {
        auto const wait_start = cc::current_time_steady_secs();
        co_await ctx.epochs_in_flight_completion(k_in_flight);
        throttled += cc::current_time_steady_secs() - wait_start;
        auto const record_start = cc::current_time_steady_secs();
        auto cmd = ctx.create_command_list();
        auto const outcome = execute(*cmd, f);
        auto const submit_start = cc::current_time_steady_secs();
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        auto const submit_end = cc::current_time_steady_secs();
        recording += submit_start - record_start;
        handing_over += submit_end - submit_start;
        REQUIRE(outcome == sg::routine_outcome::executed);
    }
    auto const cpu_done = cc::current_time_steady_secs();
    cc::rec::stop_sampling();
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(capture_handle);
    auto const captured = capture.take().spliced_samples();
    CC_LOG_INFO("{} {} back to back, where the CPU's time went:\n{}", name, c.name,
                cc::rec::hot_functions(captured).to_string(30));
    write_trace(captured, cc::format("{}-{}", name, c.name));
    co_await ctx.idle_completion();
    auto const wall = cc::current_time_steady_secs() - wall_start;
    CC_LOG_INFO("{} {} back to back on the CPU: {} ms recording, {} ms submitting and {} ms waiting for the "
                "GPU per frame; the GPU finished {} ms after the last submit",
                name, c.name, 1e3 * recording / f64(k_frames), 1e3 * handing_over / f64(k_frames),
                1e3 * throttled / f64(k_frames), 1e3 * (wall_start + wall - cpu_done));

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
    CC_LOG_INFO("{} {}x{} moving: {} {} ms per frame alone on the GPU, {} ms back to back, drawn at {}x{}", name,
                output_size[0], output_size[1], c.name, 1e3 * total / f64(k_frames), 1e3 * wall / f64(k_frames),
                input[0], input[1]);
    for (auto const& st : stages)
        CC_LOG_INFO("    {}: {} ms", st.name, 1e3 * st.seconds / f64(k_frames));
    co_await cc::async_settled(sv::background_work(ctx));
}

/// Draws `asset` moving along the longer horizontal axis of its box, at `eye_height` above its floor, every contender's way, and logs each.
/// `eye_height` is in the asset's own units, or where it is none 5% of the box's height, for an asset whose unit is unknown.
cc::shared_async<cc::unit> measure(sg::context& ctx,
                                   cc::string_view name,
                                   sv::asset_data const& asset,
                                   cc::optional<f64> eye_height)
{
    // What this process holds on the GPU against what it may, at each step: the differences are what each step costs.
    auto const memory = [&](cc::string_view when)
    {
        auto const usage = ctx.metrics.query_gpu_memory();
        if (usage.has_value())
            CC_LOG_INFO("gpu memory {}: {} MiB of a {} MiB budget", when, usage.value().current_usage_bytes >> 20,
                        usage.value().budget_bytes >> 20);
    };
    memory("with the context alone");

    auto const box = asset.bounds();
    REQUIRE(box.has_value());

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto items = cc::vector<sv::scene_item>();
    for (auto const& mesh : asset.meshes)
        items.push_back(resources.acquire_scene_item(mesh));
    resources.wait_for_pending_uploads();
    memory(cc::format("with {} uploaded", name));

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
        for (auto const samples : {2, 4})
            contenders.push_back({.name = samples == 2 ? "hybrid deferred, 2 moving samples, dlss_rr"
                                                       : "hybrid deferred, 4 moving samples, dlss_rr",
                                  .settings = {.mode = sv::render_mode::hybrid,
                                               .hybrid_lighting = sv::hybrid_lighting::deferred,
                                               .hybrid_moving_samples = samples,
                                               .reconstruct = dlss}});
        contenders.push_back({.name = "hybrid deferred, two bounces, dlss_rr",
                              .settings = {.mode = sv::render_mode::hybrid,
                                           .hybrid_lighting = sv::hybrid_lighting::deferred,
                                           .hybrid_bounces = 2,
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
    auto const eye_y = f64(lo[1]) + eye_height.value_or(0.05 * f64(hi[1] - lo[1]));
    auto const camera_at = [&](int frame)
    {
        auto const t = 0.15 + 0.7 * f64(frame) / f64(k_frames);
        auto const center = tg::pos3d(0.5 * (f64(lo[0]) + f64(hi[0])), eye_y, 0.5 * (f64(lo[2]) + f64(hi[2])));
        auto const position = along_x ? tg::pos3d(f64(lo[0]) + t * f64(hi[0] - lo[0]), eye_y, center[2])
                                      : tg::pos3d(center[0], eye_y, f64(lo[2]) + t * f64(hi[2] - lo[2]));
        auto const ahead = along_x ? tg::vec3d(1, 0, 0) : tg::vec3d(0, 0, 1);
        auto cam = sv::camera::looking_at(position, position + ahead);
        cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
        return cam;
    };

    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float);

    for (auto const& c : contenders)
        co_await time_contender(ctx, resources, name, c, items, {}, sv::background::studio(), k_output, camera_at);

    co_await cc::async_settled(sv::background_work(ctx));
    co_await ctx.idle_completion();
    co_return;
}
} // namespace

ASYNC_BENCHMARK("sv - the hybrid's whole moving frame on Sponza, denoised and upscaled to 1920x1200")
{
    auto ctx = tracing_context();
    if (ctx.has_error())
        SKIP(ctx.error().to_string());
    auto const asset = sv::asset_loader().load(nx::source_path(SV_TEST_SPONZA_DIR "/NewSponza_Main_glTF_003.gltf"));
    if (asset.has_error())
        SKIP(cc::format("Sponza did not load ({}); uv run extern/sponza/fetch-sponza.py installs it",
                        asset.error().to_string()));
    co_await measure(*ctx.value(), "sponza", asset.value(), 1.7);
}

// A street of buildings, many small pieces, and alpha-tested foliage and signs: what an opacity micromap would decide.
ASYNC_BENCHMARK("sv - the hybrid's whole moving frame on Bistro, denoised and upscaled to 1920x1200")
{
    auto ctx = tracing_context();
    if (ctx.has_error())
        SKIP(ctx.error().to_string());
    auto const asset = sv::asset_loader().load(nx::source_path(SV_TEST_BISTRO_DIR "/BistroExterior.fbx"));
    if (asset.has_error())
        SKIP(cc::format("Bistro did not load ({}); uv run extern/bistro/fetch-bistro.py installs it",
                        asset.error().to_string()));
    co_await measure(*ctx.value(), "bistro exterior", asset.value(), {});
}

// The White Room at 1920x1080 as research-studio draws it by default: the hybrid, deferred, at the native scale, the
// automatic denoiser run on every frame, under the room's own lights and from its own camera.
// Held still, as a viewer mostly is, and orbiting slowly, which restarts the mean on every frame.
ASYNC_BENCHMARK("sv - the hybrid's whole frame in the White Room at 1920x1080, still and orbiting")
{
    auto ctx = tracing_context();
    if (ctx.has_error())
        SKIP(ctx.error().to_string());
    auto const asset = sv::asset_loader().load(SV_TEST_RESEARCH_SCENES_DIR "/living-room-2/living_room_2_extended.glb");
    if (asset.has_error())
        SKIP(cc::format("the White Room did not load ({}); uv run extern/gltf-research-scenes/fetch-research-scenes.py "
                        "installs it",
                        asset.error().to_string()));
    REQUIRE(!asset.value().cameras.empty());
    auto& context = *ctx.value();

    auto resources = sv::gpu_resource_manager::create(context);
    auto items = cc::vector<sv::scene_item>();
    for (auto const& mesh : asset.value().meshes)
        items.push_back(resources.acquire_scene_item(mesh));
    resources.wait_for_pending_uploads();
    auto lights = cc::vector<sv::scene_light>();
    for (auto const& l : asset.value().lights)
        lights.push_back({.id = sv::light_id::from_string(l.id), .light = l.light});
    sr::reconstruct_routine::prewarm(context, sg::pixel_format::rgba16_float);

    auto const home = asset.value().cameras[0].camera;
    auto const still = [&](int) { return home; };
    // A degree of yaw a frame about the camera's own position: every frame sees a different image.
    auto const orbiting = [&](int frame)
    {
        auto cam = home;
        auto const turn = tg::angle_d::make_from_degree(f64(frame));
        auto const forward = home.basis().forward;
        auto const c = tg::cos(turn);
        auto const s = tg::sin(turn);
        cam = sv::camera::looking_at(
            home.position,
            home.position + tg::vec3d(forward[0] * c - forward[2] * s, forward[1], forward[0] * s + forward[2] * c));
        cam.projection = home.projection;
        return cam;
    };

    auto const settings = sv::render_settings{.mode = sv::render_mode::hybrid,
                                              .hybrid_lighting = sv::hybrid_lighting::deferred,
                                              .reconstruct = {.denoiser = sr::denoise_method::automatic},
                                              .temporal_denoise_frames = u32(-1),
                                              .motion_hold_ms = 0.0f};
    constexpr auto output = tg::vec2i(1920, 1080);
    co_await time_contender(context, resources, "white room",
                            {.name = "still, hybrid deferred, automatic", .settings = settings}, items, lights,
                            sv::background{}, output, still);
    co_await time_contender(context, resources, "white room",
                            {.name = "orbiting, hybrid deferred, automatic", .settings = settings}, items, lights,
                            sv::background{}, output, orbiting);
    // The same orbit traced at the quality scale's input extent and upscaled, as a viewer that keeps moving would.
    auto scaled = settings;
    scaled.reconstruct.scale = sr::render_scale_preset::quality;
    co_await time_contender(context, resources, "white room",
                            {.name = "orbiting at the quality scale, hybrid deferred, automatic", .settings = scaled},
                            items, lights, sv::background{}, output, orbiting);
    co_await cc::async_settled(sv::background_work(context));
    co_await context.idle_completion();
}
