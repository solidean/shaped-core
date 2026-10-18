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

// The Cornell box through the wavefront, and the run of ReSTIR frames the wavefront's tests measure, shared by the files that hold them.

namespace sv_test::wavefront
{
struct cornell_scene;
struct restir_run;
struct restir_reuse;

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

/// What a run of ReSTIR frames made of the Cornell box: each frame's re-shaded image, and their mean.
struct sv_test::wavefront::restir_run
{
    cc::vector<cc::vector<tg::vec3f>> frames;
    cc::vector<tg::vec3f> mean;
    /// Each frame's indirect half alone, the re-shaded image less the visible point's own light, and their mean.
    /// It is what ReSTIR GI resamples, without the next-event noise it leaves as it was; meaningless once DI replaces part of that light.
    cc::vector<cc::vector<tg::vec3f>> indirect_frames;
    cc::vector<tg::vec3f> indirect_mean;
    /// The mean confidence of the last frame's reservoirs, over the pixels that kept one: how much reuse there was.
    f32 confidence = 0.0f;
    /// Per pixel, the frames whose camera ray met a surface that reflects nothing: an emitter, or no surface at all.
    /// Such a pixel's image jitters between the emission and the surface beside it, which is no estimator's noise.
    cc::vector<i32> emitter_seen = {};
    /// With ReSTIR DI, the last frame's temporal DI reservoirs that kept a sample: their mean confidence, and the share on an emissive instance.
    /// A history kept from frame to frame lifts the confidence toward its cap; one dropped every frame leaves it at 1.
    f32 di_confidence = 0.0f;
    f32 di_mesh_share = 0.0f;
};

/// How a run of ReSTIR frames reuses: whether each frame may take the last one's reservoirs, and whether it traces the reverse visibility.
struct sv_test::wavefront::restir_reuse
{
    bool history = true;
    bool exact = false;
    /// How far the camera swings sideways, between two positions it alternates every frame.
    f64 pan = 0.0;
    /// Whether each frame is told the camera of the last one, or told that nothing moved.
    bool honest_motion = true;
    /// Whether each frame also resamples with a few neighbors' reservoirs, and from how far, 0 for the image's own radius.
    bool spatial = false;
    i32 radius = 0;
    /// Which run of seeds the frames draw, so a reference run shares none of the frames measured against it.
    u32 seeds = 0;
    /// Whether the re-shade also replaces the visible point's next-event light sample by ReSTIR DI's.
    bool di = false;
    /// How many surface bounces a path takes.
    i32 max_bounces = k_max_bounces;
    /// Where not empty, frame `f` traces the scene's instances as `placements[f % placements.size()]` places them, in place of `scene.instances`.
    /// Each must hold the scene's instances in their order; only where they stand may differ.
    cc::span<cc::vector<sg::tlas_instance> const> placements = {};
};

namespace sv_test::wavefront
{
/// Traces `frames` drained frames of the Cornell box, each keeping ReSTIR's records, and reads back what the re-shade made of each.
/// A pixel the re-shade did not shade shows its path's own estimate, as resolve hands the denoiser.
/// Every frame draws its own seed, so the run is a sequence of independent frames tied together only by the reuse.
inline cc::shared_async<restir_run> run_restir(sg::context& ctx,
                                               sv::gpu_resource_manager& resources,
                                               cornell_scene const& scene,
                                               int frames,
                                               restir_reuse reuse)
{
    for (auto const& placement : reuse.placements)
        REQUIRE(placement.size() == scene.instances.size());
    auto const background = sv::background_gpu::from(sv::background{});
    auto const target = make_target(ctx);
    auto state = sv::wavefront_state::create(ctx, k_size, 4);
    auto const slots = isize(k_size[0]) * isize(k_size[1]);
    auto out = restir_run{.frames = {},
                          .mean = cc::vector<tg::vec3f>::create_filled(slots, tg::vec3f(0, 0, 0)),
                          .indirect_frames = {},
                          .indirect_mean = cc::vector<tg::vec3f>::create_filled(slots, tg::vec3f(0, 0, 0)),
                          .emitter_seen = cc::vector<i32>::create_filled(slots, 0)};
    auto const start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        auto emitters = cc::vector<sv::shaders::tracer::emitter_record>();
        auto const instances = reuse.placements.empty()
                                 ? cc::span<sg::tlas_instance const>(scene.instances)
                                 : cc::span<sg::tlas_instance const>(reuse.placements[f % reuse.placements.size()]);
        if (!describe_scene(*cmd, resources, scene, instances, records, emitters))
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
            .instances = instances,
            .output = target,
            .instance_table = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer),
            .lights = sv_test::upload_lights(*cmd, scene.lights),
            .emitters = emitters.empty()
                          ? sg::buffer<sv::shaders::tracer::emitter_record>()
                          : ctx.transient.create_buffer_from_data(*cmd, emitters, sg::buffer_usage::readonly_buffer),
            .hit_groups = scene.hit_groups,
            .bindless = &bindless};
        auto fc = frame_of(scene, u32(f) * 7u + 3u + reuse.seeds * 100003u, 0u, 1);
        fc.max_bounces = reuse.max_bounces;
        auto camera = scene.camera;
        camera.position[0] += reuse.pan * f64(f % 2);
        auto previous = scene.camera;
        previous.position[0] += f == 0 ? 0.0 : reuse.pan * f64((f - 1) % 2);
        fc.camera = sv::camera_record_of(camera);
        fc.previous_camera = sv::camera_record_of(reuse.honest_motion ? previous : camera);
        auto drained = sg::data_future<u32>();
        auto const outcome = sv::wavefront_routine::execute(*cmd, {.trace = trace,
                                                                   .frame = fc,
                                                                   .state = &state,
                                                                   .rounds = drain_rounds_of(reuse.max_bounces),
                                                                   .restart = true,
                                                                   .regenerate = false,
                                                                   .restir = true,
                                                                   .restir_history = reuse.history,
                                                                   .restir_exact = reuse.exact,
                                                                   .restir_spatial = reuse.spatial,
                                                                   .restir_radius = reuse.radius,
                                                                   .restir_di = reuse.di,
                                                                   .lights_key = scene.lights.key(emitters, records),
                                                                   .live_paths = &drained});
        auto results = sg::data_future<sv::shaders::tracer::restir_result>();
        auto tallies = sg::data_future<sv::shaders::tracer::pixel_tally>();
        auto reservoirs = sg::data_future<sv::shaders::tracer::packed_reservoir>();
        auto candidates = sg::data_future<sv::shaders::tracer::packed_candidate>();
        auto di_reservoirs = sg::data_future<sv::shaders::tracer::packed_di_reservoir>();
        // The frame writes the half its parity named before it flipped it.
        auto const written_half = isize(state.restir_parity ^ 1u);
        if (outcome == sg::routine_outcome::executed)
        {
            results = cmd->download.data_from_buffer(state.restir_results);
            tallies = cmd->download.data_from_buffer(state.tallies);
            reservoirs = cmd->download.data_from_buffer(state.restir_reservoirs);
            candidates = cmd->download.data_from_buffer(state.restir_candidates);
            if (reuse.di)
                di_reservoirs = cmd->download.data_from_buffer(state.restir_di_reservoirs);
        }
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
        check_drained(drained, cc::format("ReSTIR frame {}", f));
        auto const r = results.try_get_data();
        auto const t = tallies.try_get_data();
        REQUIRE(r.has_value());
        REQUIRE(t.has_value());
        auto const c = candidates.try_get_data();
        REQUIRE(c.has_value());
        auto image = cc::vector<tg::vec3f>();
        auto indirect = cc::vector<tg::vec3f>();
        for (auto i = isize(0); i < slots; ++i)
        {
            auto const own = t.value()[i].frame_sum / cc::max(t.value()[i].frame_count, 1.0f);
            auto const reshaded = r.value()[i].valid != 0;
            auto const pixel = reshaded ? r.value()[i].radiance : own;
            auto const reflected = reshaded ? pixel - c.value()[i].direct : tg::vec3f(0, 0, 0);
            image.push_back(pixel);
            indirect.push_back(reflected);
            out.mean[i] += pixel / f32(frames);
            out.indirect_mean[i] += reflected / f32(frames);
            auto const& tally = t.value()[i];
            if (luminance_of(tally.albedo_sum) + luminance_of(tally.specular_albedo_sum) <= 1e-6f * tally.guide_count)
                ++out.emitter_seen[i];
        }
        out.frames.push_back(cc::move(image));
        out.indirect_frames.push_back(cc::move(indirect));
        auto const kept = reservoirs.try_get_data();
        REQUIRE(kept.has_value());
        auto confidence_sum = 0.0f;
        auto keeping = 0;
        for (auto i = isize(0); i < slots; ++i)
        {
            auto const& reservoir = kept.value()[written_half * slots + i];
            if (reservoir.weight <= 0.0f)
                continue;
            confidence_sum += f32(reservoir.confidence_flags & 255u);
            ++keeping;
        }
        out.confidence = confidence_sum / f32(cc::max(keeping, 1));
        if (reuse.di)
        {
            auto const di_kept = di_reservoirs.try_get_data();
            REQUIRE(di_kept.has_value());
            auto di_confidence_sum = 0.0f;
            auto di_keeping = 0;
            auto on_mesh = 0;
            for (auto i = isize(0); i < slots; ++i)
            {
                auto const& reservoir = di_kept.value()[written_half * slots + i];
                if (reservoir.weight <= 0.0f)
                    continue;
                di_confidence_sum += f32(reservoir.light_confidence >> 24u);
                ++di_keeping;
                if ((reservoir.light_confidence & 0xffffffu) >= scene.lights.path_offset[sv::light_path_mesh])
                    ++on_mesh;
            }
            out.di_confidence = di_confidence_sum / f32(cc::max(di_keeping, 1));
            out.di_mesh_share = f32(on_mesh) / f32(cc::max(di_keeping, 1));
        }
        ++f;
    }
    co_return out;
}

/// The root mean square of `image`'s luminance against `reference`'s, over the pixels that see the box's surfaces.
/// The lamp's own pixels are left out: its edge jitters from frame to frame, and that noise is no estimator's.
/// So, where `emitter_seen` is given, is every pixel whose camera ray met an emitter in any of the frames it counts.
[[nodiscard]] inline f32 rms_error(cc::span<tg::vec3f const> image,
                                   cc::span<tg::vec3f const> reference,
                                   cc::span<i32 const> emitter_seen = {})
{
    auto sum = 0.0f;
    auto count = 0;
    for (auto i = isize(0); i < image.size(); ++i)
    {
        if (luminance_of(reference[i]) > 2.0f)
            continue;
        if (!emitter_seen.empty() && emitter_seen[i] > 0)
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
