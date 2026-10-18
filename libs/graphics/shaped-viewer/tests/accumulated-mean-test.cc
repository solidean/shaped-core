#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The accumulated target holds the mean of every frame folded into it, read back pixel by pixel.
//
// Every other tracing test pins the accumulation POLICY — what restarts the counter — or a converged mean against physics.
// Neither notices a blend that weighs frames unequally, or a target that holds only the latest frame on a scene whose mean is flat.
// `frame_output` is that frame's estimate alone, so the mean of the frames read back is exactly what the target must hold.

namespace
{
using namespace cc::primitive_defines;

/// One `rgba16_float` texel as `frame_output` holds it.
struct rgba16
{
    tg::f16 r = tg::f16(0.0f);
    tg::f16 g = tg::f16(0.0f);
    tg::f16 b = tg::f16(0.0f);
    tg::f16 a = tg::f16(0.0f);
};
} // namespace

ASYNC_INVOCABLE_TEST("sv::pathtrace_routine - the accumulated target is the mean of every frame folded into it",
                     (sg::context_handle const& ctx_h))
{
    // The inline readback path, which volumetric-furnace-test documents as fastfailing on Windows on ARM.
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

    auto const box = sv_test::make_cornell_box();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("cornell box", box.positions, box.materials));
    resources.wait_for_pending_uploads();

    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const lights = sv_test::light_table_of(box.light);

    auto const size = tg::vec2i(32, 32);
    auto const pixel_count = isize(size[0]) * isize(size[1]);

    // PERSISTENT, because the target has to survive the epoch advance between the frames it accumulates.
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
    auto const frame_output = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
    auto const guide_motion = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rg32_float, .width = size[0], .height = size[1], .usage = sg::texture_usage::image});

    // Few samples per frame, so consecutive frames differ by far more than the comparison's tolerance.
    auto const frames = 4;
    auto frame_sum = cc::vector<tg::vec3f>::create_filled(pixel_count, tg::vec3f(0, 0, 0));
    auto last_frame = cc::vector<tg::vec3f>::create_filled(pixel_count, tg::vec3f(0, 0, 0));
    auto accumulated = cc::vector<tg::vec3f>();

    // A declined frame integrated nothing, so it must not count: the trace declines until its state object lands.
    auto const loop_start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();

        auto fc = sv::default_frame_constants();
        fc.camera = sv::camera_record_of(cam);
        fc.previous_camera = fc.camera;
        lights.describe_in(fc);
        fc.samples_per_pixel = 2;
        fc.max_bounces = 5;
        fc.rng_seed = u32(f) + 1u;
        fc.accum_frame = u32(f);
        fc.write_temporal = 1u;

        auto cmd = ctx.create_command_list();

        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));

        auto const frame = ctx.transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer);
        auto const background = ctx.transient.create_buffer_from_pod(*cmd, sv::background_gpu::from(sv::background{}),
                                                                     sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = sv_test::upload_lights(*cmd, lights);
        auto const bindless = resources.freeze();

        auto const traced = sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                                                  .background = background,
                                                                  .instances = instances,
                                                                  .output = target,
                                                                  .frame_output = frame_output,
                                                                  .guide_motion = guide_motion,
                                                                  .instance_table = instance_table,
                                                                  .lights = light_buffer,
                                                                  .hit_groups = hit_groups,
                                                                  .bindless = &bindless});

        // Asserted only after the submit: a REQUIRE unwinding past an unsubmitted list asserts in its destructor.
        auto const ready = traced == sg::routine_outcome::executed;

        auto frame_readback = sg::data_future<rgba16>();
        auto target_readback = sg::data_future<tg::vec4f>();
        if (ready)
        {
            frame_readback = sg::data_future<rgba16>(cmd->download.bytes_from_texture(frame_output.raw()));
            if (f == frames - 1)
                target_readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));
        }

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        // An epoch advance drains the GPU but not the readback actor, so this is the only completion guarantee.
        co_await ctx.idle_completion();

        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - loop_start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }
        ++f;

        auto const frame_pixels = frame_readback.try_get_data();
        REQUIRE(frame_pixels.has_value());
        REQUIRE(frame_pixels.value().size() == pixel_count);
        for (auto i = isize(0); i < pixel_count; ++i)
        {
            auto const& p = frame_pixels.value()[i];
            last_frame[i] = tg::vec3f(f32(p.r), f32(p.g), f32(p.b));
            frame_sum[i] += last_frame[i];
        }

        if (target_readback.is_valid())
        {
            auto const target_pixels = target_readback.try_get_data();
            REQUIRE(target_pixels.has_value());
            REQUIRE(target_pixels.value().size() == pixel_count);
            for (auto const& p : target_pixels.value())
                accumulated.push_back(tg::vec3f(p[0], p[1], p[2]));
        }
    }

    REQUIRE(accumulated.size() == pixel_count);

    // `frame_output` is half precision and the target blends the unrounded estimate, so they agree to a half's
    // relative precision, 2^-11, plus the float blend's own rounding.
    auto const tolerance = [](f32 expected) { return 2e-3f * cc::max(1.0f, tg::abs(expected)); };

    auto mismatched = 0;
    auto last_frame_differs = 0;
    for (auto i = isize(0); i < pixel_count; ++i)
    {
        auto const mean = frame_sum[i] / f32(frames);
        for (auto c = 0; c < 3; ++c)
        {
            if (tg::abs(accumulated[i][c] - mean[c]) > tolerance(mean[c]))
                ++mismatched;
        }
        if (tg::abs(last_frame[i][0] - mean[0]) > 10.0f * tolerance(mean[0]))
            ++last_frame_differs;
    }
    CHECK(mismatched == 0);

    // Without frame-to-frame noise a target holding only the last frame would pass the check above.
    CHECK(last_frame_differs > pixel_count / 4);

    co_await cc::async_settled(sv::background_work(ctx));
}
