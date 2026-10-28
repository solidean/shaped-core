#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/rendering/hybrid_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The hybrid: rasterized primary hits, lit by the tracer's next-event estimate with inline shadow rays.
//
// Both ways of lighting them are held to the megakernel's direct light on the Cornell box with its two blocks, which shadow the floor and each other.
// One frame is one light sample per pixel, so each is the mean of many frames, as the tracer's own reference is.

namespace
{
using namespace cc::primitive_defines;
using sv_test::rgba16;

/// The Cornell box lit by its rect light alone: the enclosure and both blocks, without the lamp quad standing in for the light.
/// A lamp just under the light would block every shadow ray toward it.
[[nodiscard]] sv::mesh shadowed_cornell_box()
{
    auto const box = sv_test::make_cornell_box();
    auto const triangles = box.materials.size() - sv_test::cornell_lamp_triangles;
    auto const positions
        = cc::span<tg::pos3f const>(box.positions).subspan(cc::offset_size{.offset = 0, .size = 3 * triangles});
    auto const materials
        = cc::span<sv_test::pbr_material const>(box.materials).subspan(cc::offset_size{.offset = 0, .size = triangles});
    return sv_test::as_mesh("shadowed cornell box", positions, materials);
}

/// A target a hybrid frame draws or writes, and a test reads back.
[[nodiscard]] sg::texture_2d target_of(sg::context& ctx, tg::vec2i size, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d({.format = format,
                                             .width = size[0],
                                             .height = size[1],
                                             .usage = sg::texture_usage::texture | sg::texture_usage::render_target
                                                    | sg::texture_usage::image | sg::texture_usage::copy_src});
}

/// The mean of a hybrid's frames, and the last frame's guides.
struct hybrid_readback
{
    cc::vector<tg::vec4f> color;
    cc::vector<rgba16> normal;
    cc::vector<f32> depth;
    cc::vector<rgba16> albedo;
};

/// `frames` hybrid frames of `item` lit the `lighting` way, each seeded afresh, averaged.
cc::shared_async<hybrid_readback> draw_hybrid(sg::context& ctx,
                                              sv::gpu_resource_manager& resources,
                                              sv::scene_item const& item,
                                              sv::camera const& cam,
                                              sv::pt_light_table const& lights,
                                              tg::vec2i size,
                                              sv::hybrid_lighting lighting,
                                              int frames)
{
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);
    auto permutations = cc::vector<sv::material_permutation const*>();
    permutations.push_back(permutation);
    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
    auto draws = cc::vector<sv::raster_draw>();
    draws.push_back({.instance = 0,
                     .triangle_count = u32(mesh_rec->geometry.triangle_count),
                     .permutation = 0,
                     .first_index = u32(mesh_rec->geometry.first_index),
                     .base_vertex = u32(mesh_rec->geometry.base_vertex),
                     .bounds = mesh_rec->bounds});

    auto const targets = sv::raster_gbuffer{.color = target_of(ctx, size, sg::pixel_format::rgba32_float),
                                            .normal = target_of(ctx, size, sg::pixel_format::rgba16_float),
                                            .depth = target_of(ctx, size, sg::pixel_format::r32_float),
                                            .albedo = target_of(ctx, size, sg::pixel_format::rgba16_float),
                                            .specular_albedo = target_of(ctx, size, sg::pixel_format::rgba16_float),
                                            .roughness = target_of(ctx, size, sg::pixel_format::r16_float),
                                            .motion = target_of(ctx, size, sg::pixel_format::rg32_float),
                                            .primary_depth = target_of(ctx, size, sg::pixel_format::r32_float)};
    auto const record = sv::camera_record_of(cam);
    auto visibility = sg::buffer<u32>();

    auto out = hybrid_readback{};
    auto sum = cc::vector<tg::vec4f>::create_defaulted(isize(size[0]) * isize(size[1]));
    auto const start = cc::current_time_steady_secs();
    for (auto f = 0; f < frames;)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();

        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[0], permutation->opacity);

        auto fc = sv::default_frame_constants();
        fc.camera = record;
        fc.previous_camera = record;
        lights.describe_in(fc);
        fc.near_plane = f32(cam.projection.near_plane);
        fc.pixel_spread = 2.0f * record.up_scaled.length() / f32(size[1]);
        fc.rng_seed = u32(f) + 1u;

        auto const background = ctx.transient.create_buffer_from_pod(*cmd, sv::background_gpu::from(sv::background{}),
                                                                     sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = sv_test::upload_lights(*cmd, lights);
        auto const bindless = resources.freeze();

        auto const drawn = sv::hybrid_routine::execute(*cmd, {.lighting = lighting,
                                                              .frame = fc,
                                                              .background = background,
                                                              .instances = instances,
                                                              .instance_table = instance_table,
                                                              .lights = light_buffer,
                                                              .draws = draws,
                                                              .index_pool = resources.meshes.index_pool(),
                                                              .permutations = permutations,
                                                              .bindless = &bindless,
                                                              .targets = targets,
                                                              .visibility = &visibility});

        // Read only after the submit: a REQUIRE unwinding past an unsubmitted list asserts in its destructor.
        auto const ready = drawn == sg::routine_outcome::executed;
        auto const last = f == frames - 1;
        auto color = sg::data_future<tg::vec4f>();
        auto normal = sg::data_future<rgba16>();
        auto depth = sg::data_future<f32>();
        auto albedo = sg::data_future<rgba16>();
        if (ready)
            color = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(targets.color.raw()));
        if (ready && last)
        {
            normal = sg::data_future<rgba16>(cmd->download.bytes_from_texture(targets.normal.raw()));
            depth = sg::data_future<f32>(cmd->download.bytes_from_texture(targets.depth.raw()));
            albedo = sg::data_future<rgba16>(cmd->download.bytes_from_texture(targets.albedo.raw()));
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
        auto const pixels = color.try_get_data();
        REQUIRE(pixels.has_value());
        REQUIRE(pixels.value().size() == sum.size());
        for (auto i = isize(0); i < sum.size(); ++i)
            sum[i] += pixels.value()[i];
        ++f;
        if (last)
        {
            auto const take = [](auto& future, auto& into)
            {
                auto const data = future.try_get_data();
                REQUIRE(data.has_value());
                for (auto const& v : data.value())
                    into.push_back(v);
            };
            take(normal, out.normal);
            take(depth, out.depth);
            take(albedo, out.albedo);
        }
    }
    for (auto const& s : sum)
        out.color.push_back(s / f32(frames));
    co_return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::hybrid_routine - forward and deferred light the Cornell box as the tracer's direct light "
                     "does, "
                     "shadows included",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so neither the hybrid nor its reference builds");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(shadowed_cornell_box());
    resources.wait_for_pending_uploads();

    auto const cam = sv_test::cornell_camera();
    auto const lights = sv_test::light_table_of(sv_test::make_cornell_box().light);

    constexpr auto size = tg::vec2i(64, 64);
    constexpr int block = 8;
    auto const traced = co_await sv_test::trace_direct(ctx, resources, item, cam, lights, size, 16);

    struct contender
    {
        cc::string_view name;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::forward;
    };
    contender const contenders[] = {{.name = "forward", .lighting = sv::hybrid_lighting::forward},
                                    {.name = "deferred", .lighting = sv::hybrid_lighting::deferred}};
    for (auto const& c : contenders)
    {
        auto const hybrid = co_await draw_hybrid(ctx, resources, item, cam, lights, size, c.lighting, 128);
        REQUIRE(hybrid.color.size() == traced.size());

        // Every block, every channel, over the pixels inside one face, since an edge is antialiased in the tracer's image and not here.
        // Both means are noisy, which the tolerance holds, as it holds the penumbrae a pixel's jitter moves.
        auto worst = 0.0;
        auto compared = 0;
        for (auto by = 0; by < size[1] / block; ++by)
            for (auto bx = 0; bx < size[0] / block; ++bx)
                for (auto ch = 0; ch < 3; ++ch)
                {
                    auto const t = sv_test::block_mean(traced, hybrid.normal, size, block, bx, by, ch);
                    auto const h = sv_test::block_mean(hybrid.color, hybrid.normal, size, block, bx, by, ch);
                    if (!t.has_value() || !h.has_value())
                        continue;
                    auto const err = tg::abs(h.value() - t.value()) / (t.value() + 0.05);
                    worst = cc::max(worst, err);
                    ++compared;
                    CHECK(err < 0.1).context(cc::format("{}: block ({}, {}) channel {}: hybrid {}, traced {}", c.name,
                                                        bx, by, ch, h.value(), t.value()));
                }
        CHECK(compared > 3 * 32);
        CC_LOG_INFO("{} hybrid against the tracer's direct light on the Cornell box: worst block {} over {}", c.name,
                    worst, compared);

        // The back wall straight ahead: white, facing the camera, 4.4 in front of it; the deferred kernel writes the albedo.
        auto const center = isize(size[1] / 2) * isize(size[0]) + isize(size[0] / 2);
        CHECK(tg::abs(f32(hybrid.normal[center].b) + 1.0f) < 1e-2f);
        CHECK(tg::abs(hybrid.depth[center] - 4.4f) < 1e-2f);
        CHECK(tg::abs(f32(hybrid.albedo[center].r) - 0.73f) < 1e-2f);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

// The hybrid as a view's layer: each lighting accumulates its frames until a capture's sample target, as a traced layer does.
ASYNC_INVOCABLE_TEST("sv - a hybrid layer accumulates to a capture's target, forward and deferred",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    constexpr auto size = tg::vec2i(96, 64);
    sv::hybrid_lighting const lightings[] = {sv::hybrid_lighting::forward, sv::hybrid_lighting::deferred};
    for (auto const lighting : lightings)
    {
        auto const path = cc::format("{}/sv-hybrid-capture-{}.png", cc::temp_directory_path(), int(lighting));
        auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
        auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
        auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
        auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
        auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "64");
        auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "40");

        auto const mesh = shadowed_cornell_box();
        auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
        camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

        auto const start = cc::current_time_steady_secs();
        for (auto f : sv::interactive(ctx, "sv-test/hybrid-capture"))
        {
            auto view = f.window().view();
            view.camera(camera);
            f.register_capture("front", [](sv::capture_context const&) {});
            auto scene = view.add_scene();
            scene.add_mesh(mesh);
            scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
            scene.settings({.mode = sv::render_mode::hybrid, .hybrid_lighting = lighting});
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
}
