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
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>

using namespace cc::primitive_defines;

// The megakernel against the wavefront, in GPU time per finished path, on a shallow scene and a deep one.
//
// GPU time rather than wall time: each frame is bracketed by its own timestamps, so neither the CPU's recording nor the
// gaps between submissions count.
// Per finished path rather than per frame, because the two spend a frame differently: the megakernel finishes
// `samples_per_pixel` paths per pixel, the wavefront however many its rounds bring to an end.
// The wavefront's count is its tallies' own, read back after a last frame that drains.
//
// A benchmark rather than a test: it asserts nothing, and libs/graphics/shaped-viewer/docs/wavefront.md records what it measured.

namespace
{
constexpr auto k_size = tg::vec2i(512, 512);
constexpr int k_frames = 24;

/// One scene both tracers render: a mesh, what lights it, and how deep its paths go.
struct bench_scene
{
    cc::string name;
    sv::mesh mesh;
    sv::camera camera;
    sv::background environment;
    cc::optional<sv_test::area_light> light;
    i32 max_bounces = 5;
};

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

[[nodiscard]] cc::vector<bench_scene> make_scenes()
{
    auto scenes = cc::vector<bench_scene>();
    {
        auto const box = sv_test::make_cornell_box();
        auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
        camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
        scenes.push_back({.name = "cornell box",
                          .mesh = sv_test::as_mesh("bench cornell", box.positions, box.materials),
                          .camera = camera,
                          .light = box.light});
    }
    {
        using binding = sv::material_attribute_binding;
        auto bindings = cc::vector<binding>();
        bindings.push_back(binding::of("specular_ior", 1.001f));
        bindings.push_back(binding::of("specular_roughness", 0.05f));
        bindings.push_back(binding::of("transmission_weight", 1.0f));
        bindings.push_back(binding::of("transmission_depth", 0.05f));
        bindings.push_back(binding::of("transmission_color", tg::vec3f(1, 1, 1)));
        bindings.push_back(binding::of("transmission_scatter", tg::vec3f(0.8f, 0.8f, 0.8f)));
        auto& lib = sv_test::shared_material_library();
        auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
        auto camera = sv::camera{.position = tg::pos3d(0, 0, -1.9)};
        camera.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
        scenes.push_back({.name = "dense scattering cube",
                          .mesh = {.name = "bench dense cube",
                                   .geometry = sv::triangle_geometry::create_from_positions(closed_cube(0.6f)),
                                   .material = lib.acquire(sv::material::create("bench dense", type, bindings))},
                          .camera = camera,
                          .environment = sv::background::uniform(tg::vec3f(0.5f, 0.5f, 0.5f)),
                          .max_bounces = 12});
    }
    return scenes;
}

/// The GPU seconds between each pair of timestamps, once all of them have landed.
[[nodiscard]] f64 seconds_of(cc::span<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp> const> spans)
{
    auto seconds = 0.0;
    for (auto const& [from, to] : spans)
        seconds += to.try_get_seconds().value() - from.try_get_seconds().value();
    return seconds;
}
} // namespace

ASYNC_BENCHMARK("sv - wavefront against megakernel, GPU time per path")
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    auto& ctx = *created.value();
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("the wavefront tracer needs inline ray queries");
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");
    CC_LOG_INFO("adapter: {}", ctx.metrics.adapter().name);

    // Room for one view's tallies, which is what each wavefront run reads back to count its paths.
    ctx.download.set_budget(isize(sizeof(sv::shaders::tracer::pixel_tally)) * k_size[0] * k_size[1] + (isize(1) << 20));
    ctx.advance_epoch();

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                            .width = k_size[0],
                                            .height = k_size[1],
                                            .usage = sg::texture_usage::texture | sg::texture_usage::image});

    for (auto const& scene : make_scenes())
    {
        auto const item = resources.acquire_scene_item(scene.mesh);
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
        auto const lights = scene.light.has_value() ? sv_test::light_table_of(scene.light.value()) : sv::pt_light_table();
        auto const background = sv::background_gpu::from(scene.environment);

        auto const frame_of = [&](u32 seed, u32 accum_frame, i32 spp)
        {
            auto fc = sv::default_frame_constants();
            fc.camera = sv::camera_record_of(scene.camera);
            fc.previous_camera = fc.camera;
            lights.describe_in(fc);
            fc.samples_per_pixel = spp;
            fc.max_bounces = scene.max_bounces;
            fc.rng_seed = seed;
            fc.accum_frame = accum_frame;
            return fc;
        };
        auto const trace_of = [&](sg::command_list& cmd, sg::buffer<sv::shaders::tracer::frame_constants> const& frame,
                                  sv::bound_resources const& bindless)
        {
            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
            sv::place_instance_record(records.back(), instances[0], permutation->opacity);
            return sv::pt_trace_desc{
                .frame = frame,
                .background = ctx.transient.create_buffer_from_pod(cmd, background, sg::buffer_usage::readonly_buffer),
                .instances = instances,
                .output = target,
                .instance_table = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer),
                .lights = scene.light.has_value() ? sv_test::upload_lights(cmd, lights)
                                                  : sg::buffer<sv::shaders::tracer::light_record>(),
                .hit_groups = hit_groups,
                .bindless = &bindless};
        };

        // Three contenders, interleaved frame by frame so a GPU whose clocks drift under load treats them alike:
        // the megakernel as the viewer runs it, at four samples per pixel per frame, and the wavefront at two round budgets.
        // Each wavefront run restarts on its first frame and drains on its last, so its tallies count every path it started.
        constexpr i32 spp = 4;
        constexpr int contenders = 3;
        auto const budgets = cc::fixed_array<int, 2>{8, 16};
        auto states = cc::vector<sv::wavefront_state>();
        for (auto i = 0; i < 2; ++i)
            states.push_back(sv::wavefront_state::create(ctx, k_size, 4));
        auto timings = cc::vector<cc::vector<sv::wavefront_timing>>();
        for (auto i = 0; i < 2; ++i)
            timings.push_back(cc::vector<sv::wavefront_timing>::create_defaulted(k_frames));

        auto const record = [&](sg::command_list& cmd, int contender, int f, sv::wavefront_timing* timing)
        {
            auto const bindless = resources.freeze();
            if (contender == 0)
            {
                auto const fc = frame_of(u32(f) + 1u, u32(f), spp);
                auto const frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);
                return sv::pathtrace_routine::execute(cmd, trace_of(cmd, frame, bindless));
            }
            auto const fc = frame_of(7u, 0u, 1);
            auto const frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);
            auto const last = f == k_frames - 1;
            return sv::wavefront_routine::execute(cmd, {.trace = trace_of(cmd, frame, bindless),
                                                        .frame = fc,
                                                        .state = &states[contender - 1],
                                                        .rounds = last ? scene.max_bounces + 17 : budgets[contender - 1],
                                                        .restart = f == 0,
                                                        .regenerate = !last,
                                                        .generation = 1,
                                                        .roulette_after = 3,
                                                        .timing = timing});
        };

        // Untimed until every contender has executed once, which is what its shaders compiling takes.
        auto const start = cc::current_time_steady_secs();
        for (auto c = 0; c < contenders; ++c)
        {
            auto ready = false;
            while (!ready)
            {
                (void)ctx.routines.tick();
                auto cmd = ctx.create_command_list();
                ready = record(*cmd, c, 0, nullptr) == sg::routine_outcome::executed;
                ctx.submit_command_list(cc::move(cmd));
                ctx.advance_epoch();
                co_await ctx.idle_completion();
                REQUIRE(cc::current_time_steady_secs() - start < 120.0);
                if (!ready)
                    sv_test::drive_ambient_work();
            }
        }

        auto spans = cc::vector<cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>>();
        for (auto c = 0; c < contenders; ++c)
            spans.push_back({});
        for (auto f = 0; f < k_frames; ++f)
            for (auto c = 0; c < contenders; ++c)
            {
                auto cmd = ctx.create_command_list();
                auto const from = cmd->query.record_gpu_timestamp();
                auto const outcome = record(*cmd, c, f, c == 0 ? nullptr : &timings[c - 1][f]);
                auto const to = cmd->query.record_gpu_timestamp();
                ctx.submit_command_list(cc::move(cmd));
                ctx.advance_epoch();
                REQUIRE(outcome == sg::routine_outcome::executed);
                spans[c].push_back({from, to});
            }
        co_await ctx.idle_completion();

        auto const mega_seconds = seconds_of(spans[0]);
        auto const mega_paths = f64(k_frames) * f64(spp) * f64(k_size[0]) * f64(k_size[1]);
        CC_LOG_INFO("{}: megakernel {} ns per path ({} ms)", scene.name, 1e9 * mega_seconds / mega_paths,
                    1e3 * mega_seconds);

        for (auto i = 0; i < 2; ++i)
        {
            auto cmd = ctx.create_command_list();
            auto tallies = cmd->download.data_from_buffer(states[i].tallies);
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            auto const counted = tallies.try_get_data();
            REQUIRE(counted.has_value());
            auto paths = 0.0;
            for (auto const& t : counted.value())
                paths += f64(t.count);
            auto const seconds = seconds_of(spans[i + 1]);

            // Where the wavefront's time went, summed over every frame.
            auto stages = cc::fixed_array<f64, sv::wavefront_timing::stage_count>{};
            for (auto const& t : timings[i])
            {
                auto const landed = t.try_seconds();
                REQUIRE(landed.has_value());
                for (auto s = isize(0); s < stages.size(); ++s)
                    stages[s] += landed.value()[s];
            }
            CC_LOG_INFO("{}: wavefront at {} rounds {} ns per path ({} ms), {}x the megakernel; ms per stage: begin {} "
                        "extend {} segment {} sort {} shade {} connect {} finalize {} arguments {} resolve {}",
                        scene.name, budgets[i], 1e9 * seconds / paths, 1e3 * seconds,
                        (mega_seconds / mega_paths) / (seconds / paths), 1e3 * stages[0], 1e3 * stages[1],
                        1e3 * stages[2], 1e3 * stages[3], 1e3 * stages[4], 1e3 * stages[5], 1e3 * stages[6],
                        1e3 * stages[7], 1e3 * stages[8]);
        }
    }

    co_await cc::async_settled(sv::background_work(ctx));
    co_await ctx.idle_completion();
}
