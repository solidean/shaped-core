#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/raster_routine.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/dx12/dx12_context.hh>     // sg::create_dx12_context, the reference beside vulkan
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

// The raster path: the same scenes and materials the tracer draws, rasterized and lit directly.
//
// The Cornell box is where raster and tracer must agree: lit by its ceiling alone, with nothing inside it, no wall shadows another.
// So the tracer's direct light alone — one bounce, a black environment — is the raster image, up to the tracer's noise and the raster's fixed grid over the light.
// The G-buffer is read back beside it, at points whose answer is known.

namespace
{
using namespace cc::primitive_defines;

/// One `rgba16_float` texel.
struct rgba16
{
    tg::f16 r = tg::f16(0.0f);
    tg::f16 g = tg::f16(0.0f);
    tg::f16 b = tg::f16(0.0f);
    tg::f16 a = tg::f16(0.0f);
};

/// A target a raster frame draws and a test reads back.
[[nodiscard]] sg::texture_2d target_of(sg::context& ctx, tg::vec2i size, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d(
        {.format = format,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::render_target | sg::texture_usage::copy_src});
}

[[nodiscard]] sv::raster_gbuffer gbuffer_of(sg::context& ctx, tg::vec2i size)
{
    return {.color = target_of(ctx, size, sg::pixel_format::rgba32_float),
            .normal = target_of(ctx, size, sg::pixel_format::rgba16_float),
            .depth = target_of(ctx, size, sg::pixel_format::r32_float),
            .albedo = target_of(ctx, size, sg::pixel_format::rgba16_float),
            .specular_albedo = target_of(ctx, size, sg::pixel_format::rgba16_float),
            .roughness = target_of(ctx, size, sg::pixel_format::r16_float),
            .motion = target_of(ctx, size, sg::pixel_format::rg32_float),
            .primary_depth = target_of(ctx, size, sg::pixel_format::r32_float)};
}

/// What a raster frame came back as.
struct raster_readback
{
    cc::vector<tg::vec4f> color;
    cc::vector<rgba16> normal;
    cc::vector<f32> depth;
    cc::vector<rgba16> albedo;
    cc::vector<tg::vec2f> motion;
    cc::vector<f32> primary_depth;
};

/// Draws `item` once with no transform through `raster_routine`, retrying until it executes, and reads every target back.
cc::shared_async<raster_readback> draw_raster(sg::context& ctx,
                                              sv::gpu_resource_manager& resources,
                                              sv::scene_item const& item,
                                              sv::camera const& cam,
                                              sv::pt_light_table const& lights,
                                              sv::background const& environment,
                                              tg::vec2i size)
{
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);
    auto permutations = cc::vector<sv::material_permutation const*>();
    permutations.push_back(permutation);
    auto draws = cc::vector<sv::raster_draw>();
    draws.push_back({.instance = 0, .triangle_count = u32(mesh_rec->triangle_count), .permutation = 0});

    auto const targets = gbuffer_of(ctx, size);
    auto const record = sv::camera_record_of(cam);

    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();

        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), sg::tlas_instance{}, permutation->opacity);

        auto const background = ctx.transient.create_buffer_from_pod(*cmd, sv::background_gpu::from(environment),
                                                                     sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = lights.records.empty() ? sg::buffer<sv::shaders::tracer::light_record>()
                                                         : sv_test::upload_lights(*cmd, lights);
        auto const bindless = resources.freeze();

        auto const drawn = sv::raster_routine::execute(
            *cmd, {.frame = {.camera = record,
                             .previous_camera = record,
                             .near_plane = f32(cam.projection.near_plane),
                             .light_count = u32(lights.records.size()),
                             .decal_count = 0,
                             .pixel_spread = 2.0f * record.up_scaled.length() / f32(size[1]),
                             .size = tg::vec2f(f32(size[0]), f32(size[1])),
                             .pad0 = tg::vec2f(0, 0)},
                   .background = background,
                   .instance_table = instance_table,
                   .lights = light_buffer,
                   .draws = draws,
                   .permutations = permutations,
                   .bindless = &bindless,
                   .targets = targets});

        // Asserted only after the submit: a REQUIRE unwinding past an unsubmitted list asserts in its destructor.
        auto const ready = drawn == sg::routine_outcome::executed;
        auto color = sg::data_future<tg::vec4f>();
        auto normal = sg::data_future<rgba16>();
        auto depth = sg::data_future<f32>();
        auto albedo = sg::data_future<rgba16>();
        auto motion = sg::data_future<tg::vec2f>();
        auto primary_depth = sg::data_future<f32>();
        if (ready)
        {
            color = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(targets.color.raw()));
            normal = sg::data_future<rgba16>(cmd->download.bytes_from_texture(targets.normal.raw()));
            depth = sg::data_future<f32>(cmd->download.bytes_from_texture(targets.depth.raw()));
            albedo = sg::data_future<rgba16>(cmd->download.bytes_from_texture(targets.albedo.raw()));
            motion = sg::data_future<tg::vec2f>(cmd->download.bytes_from_texture(targets.motion.raw()));
            primary_depth = sg::data_future<f32>(cmd->download.bytes_from_texture(targets.primary_depth.raw()));
        }

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }

        auto out = raster_readback{};
        auto const take = [](auto& future, auto& into)
        {
            auto const data = future.try_get_data();
            REQUIRE(data.has_value());
            for (auto const& v : data.value())
                into.push_back(v);
        };
        take(color, out.color);
        take(normal, out.normal);
        take(depth, out.depth);
        take(albedo, out.albedo);
        take(motion, out.motion);
        take(primary_depth, out.primary_depth);
        co_return out;
    }
}

/// The megakernel's direct light alone on `item` — the primary hit and its next-event estimate, nothing after — as the mean of `frames` frames.
cc::shared_async<cc::vector<tg::vec4f>> trace_direct(sg::context& ctx,
                                                     sv::gpu_resource_manager& resources,
                                                     sv::scene_item const& item,
                                                     sv::camera const& cam,
                                                     sv::pt_light_table const& lights,
                                                     tg::vec2i size,
                                                     int frames)
{
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(permutation);

    // Persistent, because the mean has to survive the epoch advance between the frames it accumulates.
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = size[0],
         .height = size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

    auto const start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();

        auto fc = sv::default_frame_constants();
        fc.camera = sv::camera_record_of(cam);
        fc.previous_camera = fc.camera;
        lights.describe_in(fc);
        fc.samples_per_pixel = 16;
        fc.max_bounces = 1;
        fc.rng_seed = u32(f) + 1u;
        fc.accum_frame = u32(f);

        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[0], permutation->opacity);

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
                                                                  .instance_table = instance_table,
                                                                  .lights = light_buffer,
                                                                  .hit_groups = hit_groups,
                                                                  .bindless = &bindless});
        auto const ready = traced == sg::routine_outcome::executed;
        auto readback = sg::data_future<tg::vec4f>();
        if (ready && f == frames - 1)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (!ready)
        {
            REQUIRE(cc::current_time_steady_secs() - start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }
        ++f;
        if (readback.is_valid())
        {
            auto const pixels = readback.try_get_data();
            REQUIRE(pixels.has_value());
            auto out = cc::vector<tg::vec4f>();
            for (auto const& p : pixels.value())
                out.push_back(p);
            co_return out;
        }
    }
    co_return {};
}

/// Whether the pixel at (`x`, `y`) sits inside one face, every neighbor seeing the same normal: what a tracer's jittered
/// samples and a raster's pixel center agree on, where an edge they would not.
[[nodiscard]] bool is_interior(cc::span<rgba16 const> normals, tg::vec2i size, int x, int y)
{
    if (x == 0 || y == 0 || x == size[0] - 1 || y == size[1] - 1)
        return false;
    auto const& n = normals[y * size[0] + x];
    for (auto dy = -1; dy <= 1; ++dy)
        for (auto dx = -1; dx <= 1; ++dx)
        {
            auto const& m = normals[(y + dy) * size[0] + (x + dx)];
            if (f32(m.r) != f32(n.r) || f32(m.g) != f32(n.g) || f32(m.b) != f32(n.b))
                return false;
        }
    return true;
}

/// The mean of channel `c` over the interior pixels of the `block` by `block` square at (`bx`, `by`), or nothing where
/// too few of them are interior to mean anything.
[[nodiscard]] cc::optional<f64> block_mean(cc::span<tg::vec4f const> img,
                                           cc::span<rgba16 const> normals,
                                           tg::vec2i size,
                                           int block,
                                           int bx,
                                           int by,
                                           int c)
{
    auto sum = 0.0;
    auto count = 0;
    for (auto y = by * block; y < (by + 1) * block; ++y)
        for (auto x = bx * block; x < (bx + 1) * block; ++x)
        {
            if (!is_interior(normals, size, x, y))
                continue;
            sum += f64(img[y * size[0] + x][c]);
            ++count;
        }
    if (count < block * block / 4)
        return {};
    return sum / f64(count);
}

/// Whether the device can run what these tests drive: the raster path itself, and the tracer it is compared against.
[[nodiscard]] bool can_run(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported();
    ctx.drop_command_list(cc::move(probe));
    return supported && sv_test::shared_env().has_compiler;
}

/// The Cornell box's enclosure alone, without its two blocks or its lamp: no wall then shadows another, so the tracer's
/// direct light is unoccluded everywhere, which the raster's always is.
/// The ceiling light is still the rect `sv_test::light_table_of` names; only the mesh standing in for it is gone.
[[nodiscard]] sv::mesh empty_cornell_box()
{
    auto const box = sv_test::make_cornell_box();
    constexpr isize enclosure_triangles = 10;
    auto const positions
        = cc::span<tg::pos3f const>(box.positions).subspan(cc::offset_size{.offset = 0, .size = 3 * enclosure_triangles});
    auto const materials = cc::span<sv_test::pbr_material const>(box.materials)
                               .subspan(cc::offset_size{.offset = 0, .size = enclosure_triangles});
    return sv_test::as_mesh("empty cornell box", positions, materials);
}

/// The camera the Cornell box tests look through: straight into the open side, square.
[[nodiscard]] sv::camera cornell_camera()
{
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    cam.projection.aspect_ratio = 1.0;
    return cam;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::raster_routine - the Cornell box's raster image is the tracer's direct light",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    if (!can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so neither the tracer nor its comparison builds");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(empty_cornell_box());
    resources.wait_for_pending_uploads();

    auto const cam = cornell_camera();
    auto const lights = sv_test::light_table_of(sv_test::make_cornell_box().light);

    constexpr auto size = tg::vec2i(64, 64);
    constexpr int block = 8;

    auto const raster = co_await draw_raster(ctx, resources, item, cam, lights, sv::background{}, size);
    auto const traced = co_await trace_direct(ctx, resources, item, cam, lights, size, 16);
    REQUIRE(raster.color.size() == isize(size[0]) * isize(size[1]));
    REQUIRE(traced.size() == raster.color.size());

    // Every block, every channel: the walls' colors are what a material evaluated wrongly would move first.
    // Over the pixels inside one face alone, since an edge is antialiased in the tracer's image and not in the raster's.
    // The tracer's mean is the reference, so its own noise is in the tolerance, and so is the raster's 4x4 grid over a light
    // the side walls' top rows sit close to.
    auto worst = 0.0;
    auto compared = 0;
    for (auto by = 0; by < size[1] / block; ++by)
        for (auto bx = 0; bx < size[0] / block; ++bx)
            for (auto c = 0; c < 3; ++c)
            {
                auto const t = block_mean(traced, raster.normal, size, block, bx, by, c);
                auto const r = block_mean(raster.color, raster.normal, size, block, bx, by, c);
                if (!t.has_value())
                    continue;
                auto const err = tg::abs(r.value() - t.value()) / (t.value() + 0.05);
                worst = cc::max(worst, err);
                ++compared;
                CHECK(err < 0.1).context(
                    cc::format("block ({}, {}) channel {}: raster {}, traced {}", bx, by, c, r.value(), t.value()));
            }
    CHECK(compared > 3 * 32);
    CC_LOG_INFO("raster against the tracer's direct light on the Cornell box: worst block {} over {}", worst, compared);

    // The back wall straight ahead: white, facing the camera, at z = 1, which is 4.4 in front of it.
    auto const center = isize(size[1] / 2) * isize(size[0]) + isize(size[0] / 2);
    auto const& n = raster.normal[center];
    CHECK(tg::abs(f32(n.r)) < 1e-2f);
    CHECK(tg::abs(f32(n.g)) < 1e-2f);
    CHECK(tg::abs(f32(n.b) + 1.0f) < 1e-2f);
    CHECK(tg::abs(raster.depth[center] - 4.4f) < 1e-2f);
    CHECK(tg::abs(f32(raster.albedo[center].r) - 0.73f) < 1e-2f);
    CHECK(tg::abs(raster.primary_depth[center] - (1.0f - f32(cam.projection.near_plane) / 4.4f)) < 1e-3f);

    // The camera did not move, so nothing did: what is left is the rounding of a reprojection, far below a pixel.
    for (auto const& m : raster.motion)
    {
        CHECK(tg::abs(m[0]) < 1e-2f);
        CHECK(tg::abs(m[1]) < 1e-2f);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv::raster_routine - a cutout material cuts what the tracer's would",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    if (!can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the shaders do not build");

    // A black, unlit quad filling the view in front of a white environment: a kept pixel is 0, and a cut one is 1.
    auto quad = cc::vector<tg::pos3f>();
    for (auto const& p : {tg::pos3f(-2, -2, 0), tg::pos3f(2, -2, 0), tg::pos3f(2, 2, 0), tg::pos3f(-2, -2, 0),
                          tg::pos3f(2, 2, 0), tg::pos3f(-2, 2, 0)})
        quad.push_back(p);
    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const quad_of = [&](cc::string_view name, f32 opacity, f32 cutoff)
    {
        using binding = sv::material_attribute_binding;
        auto const material = lib.acquire(
            sv::material::create(cc::string(name), openpbr,
                                 {binding::of("base_color", tg::vec3f(0, 0, 0)), binding::of("specular_weight", 0.0f),
                                  binding::of("opacity", opacity), binding::of("alpha_cutoff", cutoff)}));
        return sv::mesh{.name = cc::string(name),
                        .geometry = sv::triangle_geometry::create_from_positions(quad),
                        .material = material};
    };

    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const environment = sv::background::uniform(tg::vec3f(1, 1, 1));
    constexpr auto size = tg::vec2i(32, 32);

    struct cut_case
    {
        cc::string_view name;
        f32 opacity = 1.0f;
        f32 cutoff = 0.0f;
        bool cut = false;
    };
    // Above a cutoff is kept and below it cut; a cutoff of 0 is coverage, which one raster frame cuts below one half.
    auto const cases = cc::vector<cut_case>{{.name = "masked-below", .opacity = 0.3f, .cutoff = 0.5f, .cut = true},
                                            {.name = "masked-above", .opacity = 0.7f, .cutoff = 0.5f, .cut = false},
                                            {.name = "coverage-low", .opacity = 0.3f, .cutoff = 0.0f, .cut = true},
                                            {.name = "coverage-high", .opacity = 0.7f, .cutoff = 0.0f, .cut = false}};

    auto resources = sv::gpu_resource_manager::create(ctx);
    for (auto const& c : cases)
    {
        auto const item = resources.acquire_scene_item(quad_of(c.name, c.opacity, c.cutoff));
        resources.wait_for_pending_uploads();
        auto const* const permutation = resources.shaders.find(item.shader_key);
        REQUIRE(permutation != nullptr);
        REQUIRE(permutation->opacity.can_cut_out());

        auto const drawn = co_await draw_raster(ctx, resources, item, cam, {}, environment, size);
        auto wrong = 0;
        for (auto const& p : drawn.color)
        {
            auto const expected = c.cut ? 1.0f : 0.0f;
            if (tg::abs(p[0] - expected) > 1e-3f)
                ++wrong;
        }
        CHECK(wrong == 0).context(cc::format("{}: {} pixels not {}", c.name, wrong, c.cut ? "cut" : "kept"));
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a raster layer's capture finishes on its first drawn frame", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the viewer's shaders do not build");

    constexpr auto size = tg::vec2i(96, 64);
    auto const path = cc::format("{}/sv-raster-capture.png", cc::temp_directory_path());

    // Asking for the cap, which a traced layer takes thousands of frames to reach: a raster layer is there at once.
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    auto const acc
        = cc::scoped_environment_variable(sr::capture_accumulate_env_var, cc::format("{}", sv::accumulation_frame_cap));
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "40");

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);
    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    // Shader compiles are what the first frame waits on, never accumulation: a traced layer at the cap would run for
    // thousands of frames, past the timeout.
    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/raster-capture"))
    {
        auto view = f.window().view();
        view.camera(camera);
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
        scene.settings({.mode = sv::render_mode::raster});
        REQUIRE(cc::current_time_steady_secs() - start < 35.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto const decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());
    auto const& img = decoded.value();
    REQUIRE(img.width == size[0]);
    REQUIRE(img.height == size[1]);

    // The left wall red and the right one green, as the tracer draws them.
    auto const at = [&](int x, int y, int c) { return int(img.pixels[(y * img.width + x) * img.channels + c]); };
    CHECK(at(22, 32, 0) > at(22, 32, 1) + 20);
    CHECK(at(74, 32, 1) > at(74, 32, 0) + 20);
    cc::remove_file(path);
}

#if SV_TEST_HAS_VULKAN
// The raster path on vulkan, whose context this test brings up itself, held to dx12 hardware's image of the same frame.
//
// One SGL source serves both: the vertex stage pulling triangles, the generated pixel stage, the lights and the G-buffer.
// So inside every face the two images agree to rounding; an edge is left out, since the two rasterizers may cover it apart.
ASYNC_TEST("sv vulkan - the raster path draws what it draws on dx12, to rounding")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    if (!can_run(*ctx.value()))
        SKIP("no ray tracing or no SGL compiler on the vulkan device");

    // The debug layer has to be active before the process's first dx12 device, and this test may race dx12-entry.cc's
    // drivers to it, so the reference asks for the layer exactly as they do.
    auto reference = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (reference.has_error() || !can_run(*reference.value()))
        SKIP("no dx12 hardware device to hold the vulkan image to");

    // A draw's bindings and bindless elements fresh from this list's uploads need a barrier the first draw finds, inside
    // the scope, which vulkan's rendering cannot hold: sg has no way to record a draw's accesses before the scope opens.
    nx::allow_warnings("was closed and reopened around a barrier");

    constexpr auto size = tg::vec2i(64, 64);
    auto const lights = sv_test::light_table_of(sv_test::make_cornell_box().light);
    auto const draw_on = [&](sg::context& on) -> cc::shared_async<raster_readback>
    {
        auto resources = sv::gpu_resource_manager::create(on);
        auto const item = resources.acquire_scene_item(empty_cornell_box());
        resources.wait_for_pending_uploads();
        auto out = co_await draw_raster(on, resources, item, cornell_camera(), lights, sv::background::studio(), size);
        co_await cc::async_settled(sv::background_work(on));
        co_return out;
    };

    auto const own = co_await draw_on(*ctx.value());
    auto const held = co_await draw_on(*reference.value());
    REQUIRE(own.color.size() == held.color.size());

    auto compared = 0;
    auto worst = 0.0f;
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            if (!is_interior(held.normal, size, x, y) || !is_interior(own.normal, size, x, y))
                continue;
            ++compared;
            auto const i = y * size[0] + x;
            for (auto c = 0; c < 3; ++c)
            {
                auto const d = tg::abs(own.color[i][c] - held.color[i][c]) / cc::max(1e-3f, tg::abs(held.color[i][c]));
                worst = cc::max(worst, d);
            }
            CHECK(tg::abs(own.depth[i] - held.depth[i]) < 1e-4f * cc::max(1.0f, held.depth[i]));
        }
    CHECK(compared > size[0] * size[1] / 2);
    CHECK(worst < 1e-3f).context(cc::format("worst relative color difference {} over {} pixels", worst, compared));
    CC_LOG_INFO("cross-backend raster: worst relative color difference {} over {} pixels", worst, compared);
    CHECK(!ctx.value()->is_device_lost());
}
#endif
