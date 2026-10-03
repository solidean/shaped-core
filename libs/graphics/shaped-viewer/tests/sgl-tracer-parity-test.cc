#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/sgl_pathtrace_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

using namespace cc::primitive_defines;

// The SGL tracer against the HLSL one: one scene, one camera, one seed, traced by both, and the two images compared.
//
// Both shade every surface with sv's fallback material, the HLSL one through the permutation the shader cache generates and the SGL one through
// `sgl_pathtrace_routine::fallback_hit_group_source`.
// They draw the same random stream and run the same math, so what the comparison measures is the port.
//
// They agree to rounding rather than to the bit, and the cause is named: DXC compiles without `-Gis`, so every float operation carries LLVM's fast-math flags.
// The SGL text reaches DXC in another shape than the HLSL does, so the environment's MIS weight is reassociated differently and rounds a unit or two apart.
// Compiled with `-Gis` both images are bit-identical, and so are they without the environment, or with one bounce, where that weight is 1.
// That weight scales a contribution and steers no branch, so the difference stays at rounding and never forks a path.
//
// The scene is open, so rays escape to the environment, and lit by one rect light both strategies of the MIS reach.
// One of its meshes stands under a rotated, non-uniformly scaled placement, so the normal goes through the inverse transpose.

namespace
{
/// What both tracers integrate.
struct parity_scene
{
    cc::vector<sv::scene_item> items;
    cc::vector<sg::tlas_instance> instances;
    sv::camera camera;
    sv::pt_light_table lights;
    sv::background environment;
};

/// The images both tracers produced in one frame, empty where either never traced before the deadline.
struct parity_images
{
    cc::vector<tg::vec4f> hlsl;
    cc::vector<tg::vec4f> sgl;
};

constexpr i32 image_size = 48;
constexpr i32 samples_per_pixel = 8;
constexpr i32 max_bounces = 4;

/// The meshes, acquired and uploaded, and their placements; empty when one did not build.
[[nodiscard]] cc::optional<parity_scene> make_parity_scene(sv::gpu_resource_manager& resources)
{
    auto const gray = sv::pbr_material{.base_color = tg::vec3f(0.5f, 0.5f, 0.5f), .roughness = 1.0f};

    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-3, 0, -3), tg::pos3f(-3, 0, 3), tg::pos3f(3, 0, 3), tg::pos3f(3, 0, -3),
                          gray);

    auto box = sv_test::cornell_box{};
    sv_test::cb_push_box(box, tg::pos3f(-0.9f, 0.0f, -0.2f), tg::pos3f(-0.1f, 0.8f, 0.6f), gray);

    // A panel the placement below turns and stretches, so its normal goes through the inverse transpose.
    auto panel = sv_test::cornell_box{};
    sv_test::cb_push_quad(panel, tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(1, 1, 0), tg::pos3f(0, 1, 0), gray);

    auto scene = parity_scene{};
    scene.items.push_back(
        resources.acquire_scene_item(sv_test::as_mesh("parity floor", floor.positions, floor.materials)));
    scene.items.push_back(resources.acquire_scene_item(sv_test::as_mesh("parity box", box.positions, box.materials)));
    scene.items.push_back(
        resources.acquire_scene_item(sv_test::as_mesh("parity panel", panel.positions, panel.materials)));
    resources.wait_for_pending_uploads();

    // Row-major 3x4: the floor and the box at identity, and the panel turned 30 degrees about y, twice as tall as wide.
    float const placements[3][12] = {
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
        {0.8660254f, 0, 0.5f, 0.3f, 0, 2.0f, 0, 0, -0.5f, 0, 0.8660254f, 0.5f},
    };
    for (auto i = isize(0); i < scene.items.size(); ++i)
    {
        auto const* const mesh = resources.meshes.get_ptr(scene.items[i].mesh);
        if (mesh == nullptr)
            return {};
        auto instance = sg::tlas_instance{.blas = mesh->blas, .instance_id = u32(i), .hit_group_offset = 0};
        for (auto k = 0; k < 12; ++k)
            instance.transform[k] = placements[i][k];
        scene.instances.push_back(cc::move(instance));
    }

    scene.camera = sv::camera{.position = tg::pos3d(0.4, 1.6, -4.0)};
    scene.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    scene.lights = sv_test::light_table_of(
        {.center = tg::vec3f(0.2f, 2.5f, 0.3f), .half_x = 0.6f, .half_z = 0.4f, .emission = tg::vec3f(6.0f, 5.5f, 5.0f)});
    scene.environment = sv::background::gradient(tg::vec3f(0.3f, 0.4f, 0.6f), tg::vec3f(0.05f, 0.04f, 0.03f));
    return scene;
}

/// Traces `scene` once with each tracer, in one frame and from one frame block, and hands back both images.
/// Parameters by value, as a coroutine's must be.
cc::shared_async<parity_images> trace_both(sg::context* ctx, sv::gpu_resource_manager* resources, parity_scene scene)
{
    auto fc = sv::pt_frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(scene.camera);
    fc.previous_camera = fc.camera;
    scene.lights.describe_in(fc);
    fc.samples_per_pixel = samples_per_pixel;
    fc.max_bounces = max_bounces;
    fc.seed = 7u;

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    auto const* const fallback = &resources->shaders.acquire_fallback();
    hit_groups.push_back(fallback);

    // Each tracer reads the frame block and the environment from buffers of its own, the HLSL one as constants and the SGL one as storage.
    auto const hlsl_block_usage = sg::buffer_usage::constants_buffer;
    auto const sgl_block_usage = sg::buffer_usage::readonly_buffer;
    auto const target_usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src;

    auto images = parity_images{};
    auto const loop_start = cc::current_time_steady_secs();
    while (images.hlsl.empty())
    {
        (void)ctx->routines.tick();

        auto cmd = ctx->create_command_list();
        auto records = cc::vector<sv::instance_gpu>();
        for (auto const& item : scene.items)
            records.push_back(resources->describe_instance(*cmd, item.mesh, item.instance));

        auto const environment = sv::background_gpu::from(scene.environment);
        auto const hlsl_frame = ctx->transient.create_buffer_from_pod(*cmd, fc, hlsl_block_usage);
        auto const hlsl_background = ctx->transient.create_buffer_from_pod(*cmd, environment, hlsl_block_usage);
        auto const sgl_frame = ctx->transient.create_buffer_from_pod(*cmd, fc, sgl_block_usage);
        auto const sgl_background = ctx->transient.create_buffer_from_pod(*cmd, environment, sgl_block_usage);
        auto const instance_table
            = ctx->transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = sv_test::upload_lights(*cmd, scene.lights);
        auto const make_target = [&]
        {
            return ctx->transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                     .width = image_size,
                                                     .height = image_size,
                                                     .usage = target_usage});
        };
        auto const hlsl_target = make_target();
        auto const sgl_target = make_target();

        auto const bindless = resources->freeze();
        auto const desc_for = [&](sg::buffer<sv::pt_frame_constants_gpu> const& frame,
                                  sg::buffer<sv::background_gpu> const& background, sg::texture_2d const& target)
        {
            return sv::pt_trace_desc{.frame = frame,
                                     .background = background,
                                     .instances = scene.instances,
                                     .output = target,
                                     .instance_table = instance_table,
                                     .lights = light_buffer,
                                     .hit_groups = hit_groups,
                                     .fallback = fallback,
                                     .bindless = &bindless};
        };
        auto const hlsl_outcome
            = sv::pathtrace_routine::execute(*cmd, desc_for(hlsl_frame, hlsl_background, hlsl_target));
        auto const sgl_outcome
            = sv::sgl_pathtrace_routine::execute(*cmd, desc_for(sgl_frame, sgl_background, sgl_target));
        auto const both = hlsl_outcome == sg::routine_outcome::executed && sgl_outcome == sg::routine_outcome::executed;

        auto hlsl_back = cc::optional<sg::data_future<tg::vec4f>>();
        auto sgl_back = cc::optional<sg::data_future<tg::vec4f>>();
        if (both)
        {
            hlsl_back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(hlsl_target.raw()));
            sgl_back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(sgl_target.raw()));
        }
        ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        (void)co_await ctx->idle_completion();

        if (!both)
        {
            // Not a failure yet: a hit group or a state object has not landed, and the deadline turns a hang into a message.
            if (cc::current_time_steady_secs() - loop_start > 90.0)
                co_return images;
            sv_test::drive_ambient_work();
            continue;
        }

        (void)co_await ctx->idle_completion(); // an epoch advance drains the GPU but not the readback actor
        auto const hlsl_pixels = hlsl_back.value().try_get_data();
        auto const sgl_pixels = sgl_back.value().try_get_data();
        if (!hlsl_pixels.has_value() || !sgl_pixels.has_value())
            co_return images;
        for (auto const& px : hlsl_pixels.value().span())
            images.hlsl.push_back(px);
        for (auto const& px : sgl_pixels.value().span())
            images.sgl.push_back(px);
    }
    co_return images;
}

/// How far a channel of the SGL image may stray from the HLSL one, relative to its value: eight units in the last place.
/// Rounding apart, as the header says why; a forked path would differ by the whole of a contribution instead.
constexpr float k_relative_tolerance = 8.0f * 1.1920929e-7f;

/// How far apart two images are: the largest difference of any channel, absolute and relative, and how many pixels differ at all.
struct image_difference
{
    float max_abs = 0.0f;
    float max_relative = 0.0f;
    isize differing_pixels = 0;
    float mean_hlsl = 0.0f;
};

[[nodiscard]] image_difference difference_of(cc::vector<tg::vec4f> const& hlsl, cc::vector<tg::vec4f> const& sgl)
{
    auto d = image_difference{};
    auto sum = 0.0;
    for (auto i = isize(0); i < hlsl.size(); ++i)
    {
        auto differs = false;
        for (auto c = 0; c < 3; ++c)
        {
            auto const delta = tg::abs(hlsl[i][c] - sgl[i][c]);
            d.max_abs = delta > d.max_abs ? delta : d.max_abs;
            auto const magnitude = tg::abs(hlsl[i][c]);
            auto const relative = delta == 0.0f ? 0.0f : delta / (magnitude > 1e-30f ? magnitude : 1e-30f);
            d.max_relative = relative > d.max_relative ? relative : d.max_relative;
            differs = differs || hlsl[i][c] != sgl[i][c];
            sum += hlsl[i][c];
        }
        d.differing_pixels += differs ? 1 : 0;
    }
    d.mean_hlsl = float(sum / double(hlsl.size() * 3));
    return d;
}

/// The parity check itself, on whichever context the caller brought up.
cc::shared_async<cc::unit> check_parity(sg::context* ctx, cc::string_view backend)
{
    auto resources = sv::gpu_resource_manager::create(*ctx);
    auto const scene = make_parity_scene(resources);
    REQUIRE(scene.has_value());

    auto const images = co_await trace_both(ctx, &resources, scene.value());
    REQUIRE(!images.hlsl.empty());
    REQUIRE(images.hlsl.size() == image_size * image_size);
    REQUIRE(images.sgl.size() == images.hlsl.size());

    auto const d = difference_of(images.hlsl, images.sgl);
    CC_LOG_INFO("SGL tracer parity on {}: max abs diff {}, max relative diff {}, {} of {} pixels differ, mean {}",
                backend, d.max_abs, d.max_relative, d.differing_pixels, images.hlsl.size(), d.mean_hlsl);

    // The scene is lit, or the comparison compares black with black.
    CHECK(d.mean_hlsl > 0.05f);

    CHECK(d.max_relative <= k_relative_tolerance)
        .context(cc::format("{} of {} pixels differ, by up to {} ({} relative)", d.differing_pixels, images.hlsl.size(),
                            d.max_abs, d.max_relative));

    co_await cc::async_settled(sv::background_work(*ctx));
    co_return;
}

/// Whether this device and this build can trace at all; the reason to skip when not.
[[nodiscard]] cc::optional<cc::string_view> cannot_trace(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported() && ctx.supports(sg::feature::raytracing_pipeline);
    ctx.drop_command_list(cc::move(probe));
    if (!supported)
        return cc::string_view("device reports no ray-tracing pipelines");
    if (!sv_test::shared_env().has_compiler)
        return cc::string_view("no DXC compiler to build the path-tracing shaders");
    return {};
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::sgl_pathtrace_routine - traces what pathtrace_routine traces, to rounding",
                     (sg::context_handle const& ctx_h))
{
    if (auto const reason = cannot_trace(*ctx_h); reason.has_value())
        SKIP(reason.value());
    (void)co_await check_parity(ctx_h.get(), "dx12");
}

#if SV_TEST_HAS_VULKAN
// The same comparison on vulkan, whose context this test brings up itself: sv's other GPU tests are dx12's alone.
ASYNC_TEST("sv vulkan - the SGL tracer traces what the HLSL one traces, to rounding")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    if (auto const reason = cannot_trace(*ctx.value()); reason.has_value())
        SKIP(reason.value());
    (void)co_await check_parity(ctx.value().get(), "vulkan");
    CHECK(!ctx.value()->is_device_lost());
}
#endif
