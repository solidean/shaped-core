#pragma once

#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::sqrt

// The Cornell box through the wavefront, and the run of drained frames the wavefront's tests measure, shared by the files that hold them.

namespace sv_test::wavefront
{
struct cornell_scene;
struct drained_run;
struct drained_options;

inline constexpr auto k_size = tg::vec2i(32, 32);
} // namespace sv_test::wavefront

/// The scene both tracers render: the Cornell box through the managers, its instances and its light table.
struct sv_test::wavefront::cornell_scene
{
    sv_test::cornell_box box = sv_test::make_cornell_box();
    sv::scene_item item;
    cc::vector<sg::tlas_instance> instances;
    cc::vector<sv::material_permutation const*> hit_groups;
    sv::pt_light_table lights;
    sv::camera camera;
    /// Emissive instances lighting the box beside the analytic lights, one TLAS instance each after the box's.
    /// Their records of path `light_path_mesh` stand last in `lights`, in this order, as the view links them.
    cc::vector<sv::scene_item> emitters = {};
};

namespace sv_test::wavefront
{
[[nodiscard]] inline cornell_scene make_scene(sv::gpu_resource_manager& resources,
                                              sv_test::cornell_box box = sv_test::make_cornell_box())
{
    auto s = cornell_scene{.box = cc::move(box)};
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

/// `scene`'s instance records for one recording into `records`, and its lamps' emitter records into `emitters`, linked as the view links them.
/// Each is placed as `instances` places it, which is `scene.instances` or one placement of them.
/// False while a lamp's area CDF has not been built: `cmd` then carries what builds it, and must not trace.
[[nodiscard]] inline bool describe_scene(sg::command_list& cmd,
                                         sv::gpu_resource_manager& resources,
                                         cornell_scene const& scene,
                                         cc::span<sg::tlas_instance const> instances,
                                         cc::vector<sv::shaders::tracer::instance_record>& records,
                                         cc::vector<sv::shaders::tracer::emitter_record>& emitters)
{
    records.push_back(resources.describe_instance(cmd, scene.item.mesh, scene.item.instance));
    sv::place_instance_record(records.back(), instances[0], scene.hit_groups[0]->opacity);
    auto ready = true;
    for (auto k = isize(0); k < scene.emitters.size(); ++k)
    {
        auto const& item = scene.emitters[k];
        auto const* const permutation = resources.shaders.find(item.shader_key);
        REQUIRE(permutation != nullptr);
        records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[1 + k], permutation->opacity);
        records.back().emitter = scene.lights.path_offset[sv::light_path_mesh] + u32(k);
        auto const* const cdf = resources.meshes.acquire_emitter_cdf(cmd, item.mesh);
        if (cdf == nullptr)
        {
            ready = false;
            continue;
        }
        auto const triangles = isize(resources.meshes.get_ptr(item.mesh)->triangle_count);
        auto const cdf_index = u32(resources.acquire_buffer(cdf->raw()->as_raw_readonly()));
        emitters.push_back(sv::emitter_record_of(u32(records.size() - 1), cdf_index, u32(triangles),
                                                 sv::emitter_cdf_block_count(triangles), permutation->emission));
    }
    return ready;
}

inline constexpr i32 k_max_bounces = 5;

/// The rounds a draining frame of the Cornell box runs at `max_bounces`.
/// The box holds no medium, so a path ends within `max_bounces + 1` rounds and the last round must find every slot empty.
[[nodiscard]] constexpr int drain_rounds_of(i32 max_bounces)
{
    return max_bounces + 2;
}

/// Checks that the last round of a draining frame started with no live path, so its image holds every sample it started.
/// A drain given too few rounds fails here, rather than as an image a few percent darker.
inline void check_drained(sg::data_future<u32> const& live_paths, cc::string_view frame)
{
    auto const counts = live_paths.try_get_data();
    REQUIRE(counts.has_value());
    REQUIRE(counts.value().size() > 0);
    auto const last = counts.value()[counts.value().size() - 1];
    CHECK(last == 0u).context(cc::format("{}: {} paths entered the last round", frame, last));
}

[[nodiscard]] inline sv::shaders::tracer::frame_constants frame_of(cornell_scene const& s, u32 seed, u32 accum_frame, i32 spp)
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

[[nodiscard]] inline sg::texture_2d make_target(sg::context& ctx, tg::vec2i size = k_size)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
}

[[nodiscard]] inline f32 luminance_of(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}
} // namespace sv_test::wavefront

/// What a run of drained frames made of the Cornell box: each frame's image, and their mean.
struct sv_test::wavefront::drained_run
{
    cc::vector<cc::vector<tg::vec3f>> frames;
    cc::vector<tg::vec3f> mean;
};

/// How a run of drained frames traces.
struct sv_test::wavefront::drained_options
{
    /// Which run of seeds the frames draw, so a reference run shares none of the frames measured against it.
    u32 seeds = 0;
    /// How many surface bounces a path takes.
    i32 max_bounces = k_max_bounces;
};

namespace sv_test::wavefront
{
/// Traces `frames` drained frames of the Cornell box and reads back each frame's estimate, as resolve hands the denoiser.
/// Every frame draws its own seed, so the run is a sequence of independent frames.
inline cc::shared_async<drained_run> run_drained(sg::context& ctx,
                                                 sv::gpu_resource_manager& resources,
                                                 cornell_scene const& scene,
                                                 int frames,
                                                 drained_options options = {})
{
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto const slots = isize(k_size[0]) * isize(k_size[1]);
    auto out = drained_run{.frames = {}, .mean = cc::vector<tg::vec3f>::create_filled(slots, tg::vec3f(0, 0, 0))};
    auto const start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        auto emitters = cc::vector<sv::shaders::tracer::emitter_record>();
        if (!describe_scene(*cmd, resources, scene, scene.instances, records, emitters))
        {
            // A lamp's area CDF is building on this list, which runs; the scene is traced once it has landed.
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            sv_test::drive_ambient_work();
            continue;
        }
        auto const bindless = resources.freeze();
        auto const trace = sv::pt_trace_desc{
            .background = ctx.transient.create_buffer_from_pod(*cmd, background, sg::buffer_usage::readonly_buffer),
            .instances = scene.instances,
            .output = target,
            .instance_table = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer),
            .lights = sv_test::upload_lights(*cmd, scene.lights),
            .emitters = emitters.empty()
                          ? sg::buffer<sv::shaders::tracer::emitter_record>()
                          : ctx.transient.create_buffer_from_data(*cmd, emitters, sg::buffer_usage::readonly_buffer),
            .hit_groups = scene.hit_groups,
            .bindless = &bindless};
        auto fc = frame_of(scene, u32(f) * 7u + 3u + options.seeds * 100003u, 0u, 1);
        fc.max_bounces = options.max_bounces;
        auto drained = sg::data_future<u32>();
        auto const outcome = sv::wavefront_routine::execute(*cmd, {.trace = trace,
                                                                   .frame = fc,
                                                                   .state = &state,
                                                                   .rounds = drain_rounds_of(options.max_bounces),
                                                                   .restart = true,
                                                                   .regenerate = false,
                                                                   .live_paths = &drained});
        auto tallies = sg::data_future<sv::shaders::tracer::pixel_tally>();
        if (outcome == sg::routine_outcome::executed)
            tallies = cmd->download.data_from_buffer(state.tallies);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (outcome != sg::routine_outcome::executed)
        {
            // A cold compile of every wavefront kernel takes an unoptimized build about a minute on its own.
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            sv_test::drive_ambient_work();
            continue;
        }
        check_drained(drained, cc::format("drained frame {}", f));
        auto const t = tallies.try_get_data();
        REQUIRE(t.has_value());
        auto image = cc::vector<tg::vec3f>();
        for (auto i = isize(0); i < slots; ++i)
        {
            auto const own = t.value()[i].frame_sum / cc::max(t.value()[i].frame_count, 1.0f);
            image.push_back(own);
            out.mean[i] += own / f32(frames);
        }
        out.frames.push_back(cc::move(image));
        ++f;
    }
    co_return out;
}

/// The root mean square of `image`'s luminance against `reference`'s, over the pixels that see the box's surfaces.
/// The lamp's own pixels are left out: its edge jitters from frame to frame, and that noise is no estimator's.
/// So, where `excluded` is given, is every pixel it holds above 0 for.
[[nodiscard]] inline f32 rms_error(cc::span<tg::vec3f const> image,
                                   cc::span<tg::vec3f const> reference,
                                   cc::span<i32 const> excluded = {})
{
    auto sum = 0.0f;
    auto count = 0;
    for (auto i = isize(0); i < image.size(); ++i)
    {
        if (luminance_of(reference[i]) > 2.0f)
            continue;
        if (!excluded.empty() && excluded[i] > 0)
            continue;
        auto const d = luminance_of(image[i]) - luminance_of(reference[i]);
        sum += d * d;
        ++count;
    }
    return tg::sqrt(sum / f32(cc::max(count, 1)));
}

/// The mean luminance of `pixels`.
[[nodiscard]] inline f32 mean_luminance_of(cc::span<tg::vec3f const> pixels)
{
    auto sum = 0.0f;
    for (auto const& p : pixels)
        sum += luminance_of(p);
    return sum / f32(pixels.size());
}
} // namespace sv_test::wavefront
