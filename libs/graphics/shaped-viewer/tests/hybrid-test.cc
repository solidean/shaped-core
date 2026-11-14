#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/rendering/hybrid_routine.hh>
#include <shaped-viewer/scene/studio.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The hybrid: rasterized primary hits, lit by the tracer's next-event estimate with inline shadow rays.
//
// Both ways of lighting them are held to the megakernel's two-bounce light on the Cornell box with its two blocks, which shadow the floor and each other.
// Its walls are `pbr`, whose base color, roughness and metalness the hybrid's secondary material reads, so its secondary hits are the tracer's own surfaces.
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

/// `frames` hybrid frames of `item` lit the `lighting` way under `background`, each seeded afresh, averaged.
cc::shared_async<hybrid_readback> draw_hybrid(sg::context& ctx,
                                              sv::gpu_resource_manager& resources,
                                              sv::scene_item const& item,
                                              sv::camera const& cam,
                                              sv::pt_light_table const& lights,
                                              tg::vec2i size,
                                              sv::hybrid_lighting lighting,
                                              int frames,
                                              sv::background const& background = {},
                                              int bounces = 1,
                                              int samples = 1)
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
        sv::place_instance_record(records.back(), instances[0], permutation->opacity, permutation->secondary);

        auto fc = sv::default_frame_constants();
        fc.camera = record;
        fc.previous_camera = record;
        lights.describe_in(fc);
        fc.near_plane = f32(cam.projection.near_plane);
        fc.pixel_spread = 2.0f * record.up_scaled.length() / f32(size[1]);
        fc.rng_seed = u32(f) + 1u;
        // what a hybrid frame reads as its bounce count, `render_settings::hybrid_bounces`
        fc.max_bounces = bounces;
        // and its samples per pixel, `render_settings::hybrid_moving_samples`
        fc.hybrid_samples = samples;

        auto const background_buffer = ctx.transient.create_buffer_from_pod(*cmd, sv::background_gpu::from(background),
                                                                            sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = sv_test::upload_lights(*cmd, lights);
        auto const bindless = resources.freeze();

        auto const drawn = sv::hybrid_routine::execute(*cmd, {.lighting = lighting,
                                                              .frame = fc,
                                                              .background = background_buffer,
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

ASYNC_INVOCABLE_TEST("sv::hybrid_routine - forward and deferred light the Cornell box as the tracer's first bounce "
                     "does, "
                     "shadows included",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
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
    // The tracer's path length counts the primary hit, so a hybrid of n bounces is held to the tracer's n + 1.
    // The box is matte throughout, so the secondary material the bounces light is the material itself, and only noise is left between them.
    // That noise is what the tolerance below holds: at a quarter of these samples the worst block sat at 0.10, at the limit itself.
    auto const traced_one = co_await sv_test::trace_direct(ctx, resources, item, cam, lights, size, 64, 2);
    auto const traced_two = co_await sv_test::trace_direct(ctx, resources, item, cam, lights, size, 64, 3);

    struct contender
    {
        cc::string_view name;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::forward;
        int bounces = 1;
    };
    contender const contenders[]
        = {{.name = "forward", .lighting = sv::hybrid_lighting::forward},
           {.name = "deferred", .lighting = sv::hybrid_lighting::deferred},
           {.name = "forward, two bounces", .lighting = sv::hybrid_lighting::forward, .bounces = 2},
           {.name = "deferred, two bounces", .lighting = sv::hybrid_lighting::deferred, .bounces = 2}};
    for (auto const& c : contenders)
    {
        auto const& traced = c.bounces == 2 ? traced_two : traced_one;
        auto const hybrid = co_await draw_hybrid(ctx, resources, item, cam, lights, size, c.lighting, 512, {}, c.bounces);
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
        CC_LOG_INFO("{} hybrid against the tracer at one bounce more on the Cornell box: worst block {} over {}",
                    c.name, worst, compared);

        // The back wall straight ahead: white, facing the camera, 4.4 in front of it; the deferred kernel writes the albedo.
        auto const center = isize(size[1] / 2) * isize(size[0]) + isize(size[0] / 2);
        CHECK(tg::abs(f32(hybrid.normal[center].b) + 1.0f) < 1e-2f);
        CHECK(tg::abs(hybrid.depth[center] - 4.4f) < 1e-2f);
        CHECK(tg::abs(f32(hybrid.albedo[center].r) - 0.73f) < 1e-2f);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

// Glass under a uniform sky is the furnace the tracer's own tests hold their refraction to: lossless, it is invisible.
// The hybrid's bounce crosses the sphere once and loses what its far side reflects back inside: 0.96 of the sky at the center, 0.88 over the disc.
// Without the crossing a glass primary hit showed its reflection alone, a tenth of the sky.
// A deferred frame of four samples per pixel is the mean of four: 16 such frames hold what 64 frames of one do, which only noise separates.
// A moving image lights this many to give its temporal denoiser a steadier frame, so a bias here would show as a moving image brighter or darker than a still one.
ASYNC_INVOCABLE_TEST("sv::hybrid_routine - a deferred frame of four samples per pixel is the mean of four frames of "
                     "one",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(shadowed_cornell_box());
    resources.wait_for_pending_uploads();
    auto const cam = sv_test::cornell_camera();
    auto const lights = sv_test::light_table_of(sv_test::make_cornell_box().light);

    constexpr auto size = tg::vec2i(48, 48);
    auto const ones = co_await draw_hybrid(ctx, resources, item, cam, lights, size, sv::hybrid_lighting::deferred, 64);
    auto const fours
        = co_await draw_hybrid(ctx, resources, item, cam, lights, size, sv::hybrid_lighting::deferred, 16, {}, 1, 4);
    auto const mean = [](cc::span<tg::vec4f const> image)
    {
        auto sum = 0.0;
        for (auto const& c : image)
            sum += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        return sum / f64(image.size());
    };
    auto const one = mean(ones.color);
    auto const four = mean(fours.color);
    CC_LOG_INFO("the Cornell box, deferred: {} from 64 frames of one sample, {} from 16 frames of four", one, four);
    CHECK(tg::abs(four - one) < 0.02 * one).dump("one", one).dump("four", four);
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv::hybrid_routine - a glass sphere under a uniform sky shows the sky through it",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    using binding = sv::material_attribute_binding;
    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto bindings = cc::vector<binding>();
    bindings.push_back(binding::of("specular_ior", 1.5f));
    bindings.push_back(binding::of("specular_roughness", 0.05f));
    bindings.push_back(binding::of("transmission_weight", 1.0f));
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const material = lib.acquire(sv::material::create("hybrid glass", openpbr, cc::move(bindings)));
    auto const sky = 0.5f;
    constexpr auto size = tg::vec2i(48, 48);

    auto const item = resources.acquire_scene_item(
        sv::mesh{.name = "hybrid glass sphere", .geometry = sv_test::sphere_geometry(0.6f, 24, 32), .material = material});
    resources.wait_for_pending_uploads();
    // a light that adds nothing, since the sky is the whole of the lighting
    auto const lights = sv_test::light_table_of(
        {.center = tg::vec3f(0, 50, 0), .half_x = 0.1f, .half_z = 0.1f, .emission = tg::vec3f(0, 0, 0)});

    // Close by, and from a thousand units off through a narrow lens, framing the sphere the same.
    // From far off the deferred kernel's rebuilt point is rounded to the camera's distance and lifted off the surface by that.
    // A refraction has to be lifted through the surface rather than back toward the viewer, or the glass meets its own front face.
    struct placement
    {
        f64 distance = 1.9;
        f64 fov_degrees = 60.0;
    };
    for (auto const at :
         {placement{.distance = 1.9, .fov_degrees = 60.0}, placement{.distance = 1000.0, .fov_degrees = 0.1146}})
        for (auto const lighting : {sv::hybrid_lighting::forward, sv::hybrid_lighting::deferred})
        {
            auto cam = sv::camera::looking_at(tg::pos3d(0, 0, -at.distance), tg::pos3d(0, 0, 0));
            cam.projection.vertical_fov = tg::angle_d::make_from_degree(at.fov_degrees);

            auto const name = cc::format(
                "{} from {}", lighting == sv::hybrid_lighting::forward ? "forward" : "deferred", at.distance);
            auto const hybrid = co_await draw_hybrid(ctx, resources, item, cam, lights, size, lighting, 64,
                                                     sv::background::uniform(tg::vec3f(sky, sky, sky)));

            // The middle of the image, which only the sphere covers, and the whole of its disc.
            auto center = 0.0f;
            auto center_count = 0;
            auto disc = 0.0f;
            auto disc_count = 0;
            for (auto y = 0; y < size[1]; ++y)
                for (auto x = 0; x < size[0]; ++x)
                {
                    auto const i = isize(y) * isize(size[0]) + isize(x);
                    auto const n = hybrid.normal[i];
                    if (f32(n.r) == 0.0f && f32(n.g) == 0.0f && f32(n.b) == 0.0f)
                        continue;
                    auto const c = hybrid.color[i];
                    auto const lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                    disc += lum;
                    ++disc_count;
                    if (tg::abs(x - size[0] / 2) < 6 && tg::abs(y - size[1] / 2) < 6)
                    {
                        center += lum;
                        ++center_count;
                    }
                }
            REQUIRE(center_count == 11 * 11);
            REQUIRE(disc_count > size[0] * size[1] / 4);
            center /= f32(center_count);
            disc /= f32(disc_count);
            CC_LOG_INFO("{} hybrid glass sphere under a sky of {}: {} at its center, {} over its disc", name, sky,
                        center, disc);
            CHECK(center > 0.9f * sky).context(name).dump("center", center);
            CHECK(center < 1.02f * sky).context(name).dump("center", center);
            CHECK(disc > 0.82f * sky).context(name).dump("disc", disc);
        }
    co_await cc::async_settled(sv::background_work(ctx));
}

// A closed pane half a millimeter thick, facing the camera under a uniform sky, shows the sky through it, losing only its two faces' reflections.
// Thinner than any fixed distance a continuing ray could be offset by: a crossing that stepped over the far face found no way out, and the pane went black.
ASYNC_INVOCABLE_TEST("sv::hybrid_routine - a glass pane thinner than a millimeter shows the sky through it",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    using binding = sv::material_attribute_binding;
    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto bindings = cc::vector<binding>();
    bindings.push_back(binding::of("specular_ior", 1.5f));
    bindings.push_back(binding::of("specular_roughness", 0.0f));
    bindings.push_back(binding::of("transmission_weight", 1.0f));

    // Closed and wound outward, so it is a solid the bounce crosses rather than a thin wall.
    auto const hx = 0.6f;
    auto const hz = 0.00025f;
    tg::pos3f const corners[]
        = {tg::pos3f(-hx, -hx, -hz), tg::pos3f(+hx, -hx, -hz), tg::pos3f(+hx, +hx, -hz), tg::pos3f(-hx, +hx, -hz),
           tg::pos3f(-hx, -hx, +hz), tg::pos3f(+hx, -hx, +hz), tg::pos3f(+hx, +hx, +hz), tg::pos3f(-hx, +hx, +hz)};
    int const faces[]
        = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    auto positions = cc::vector<tg::pos3f>();
    for (auto const i : faces)
        positions.push_back(corners[i]);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(
        sv::mesh{.name = "hybrid thin pane",
                 .geometry = sv::triangle_geometry::create_from_positions(positions),
                 .material = lib.acquire(sv::material::create("hybrid thin pane", openpbr, cc::move(bindings)))});
    resources.wait_for_pending_uploads();

    auto cam = sv::camera{.position = tg::pos3d(0, 0, -1.9)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
    auto const lights = sv_test::light_table_of(
        {.center = tg::vec3f(0, 50, 0), .half_x = 0.1f, .half_z = 0.1f, .emission = tg::vec3f(0, 0, 0)});
    auto const sky = 0.5f;

    constexpr auto size = tg::vec2i(48, 48);
    for (auto const lighting : {sv::hybrid_lighting::forward, sv::hybrid_lighting::deferred})
    {
        auto const name = lighting == sv::hybrid_lighting::forward ? "forward" : "deferred";
        auto const hybrid = co_await draw_hybrid(ctx, resources, item, cam, lights, size, lighting, 16,
                                                 sv::background::uniform(tg::vec3f(sky, sky, sky)));
        auto center = 0.0f;
        auto count = 0;
        for (auto y = size[1] / 2 - 6; y < size[1] / 2 + 6; ++y)
            for (auto x = size[0] / 2 - 6; x < size[0] / 2 + 6; ++x)
            {
                auto const c = hybrid.color[isize(y) * isize(size[0]) + isize(x)];
                center += 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                ++count;
            }
        center /= f32(count);
        CC_LOG_INFO("{} hybrid thin pane under a sky of {}: {} at its center", name, sky, center);
        CHECK(center > 0.85f * sky).context(name).dump("center", center);
        CHECK(center < 1.02f * sky).context(name).dump("center", center);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

// A mirror floor near y = 0, seen from a camera tens of units from the origin, reflects the sky the same forward and deferred.
// The deferred kernel rebuilds its point from the camera, rounded to the camera's distance from the origin rather than to the point's coordinates.
// Offset only as an interpolated point is, rows of it started under the floor, and their reflections hit the floor in dark stripes.
ASYNC_INVOCABLE_TEST("sv::hybrid_routine - a mirror floor near the origin, seen from far off it, reflects the sky the "
                     "same forward and deferred",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    using binding = sv::material_attribute_binding;
    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto bindings = cc::vector<binding>();
    bindings.push_back(binding::of("base_color", tg::vec3f(0, 0, 0)));
    bindings.push_back(binding::of("specular_ior", 1.5f));
    bindings.push_back(binding::of("specular_roughness", 0.0f));

    auto const y = 0.11f;
    tg::pos3f const corners[] = {tg::pos3f(0, y, -10), tg::pos3f(0, y, 40), tg::pos3f(60, y, 40), tg::pos3f(60, y, -10)};
    auto positions = cc::vector<tg::pos3f>();
    for (auto const i : {0, 1, 2, 0, 2, 3})
        positions.push_back(corners[i]);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(
        sv::mesh{.name = "hybrid mirror floor",
                 .geometry = sv::triangle_geometry::create_from_positions(positions),
                 .material = lib.acquire(sv::material::create("hybrid mirror floor", openpbr, cc::move(bindings)))});
    resources.wait_for_pending_uploads();

    auto cam = sv::camera::looking_at(tg::pos3d(30, 15, -30), tg::pos3d(30, 0.11, 10));
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
    auto const lights = sv_test::light_table_of(
        {.center = tg::vec3f(0, 50, 0), .half_x = 0.1f, .half_z = 0.1f, .emission = tg::vec3f(0, 0, 0)});
    auto const sky = sv::background::uniform(tg::vec3f(0.5f, 0.5f, 0.5f));

    constexpr auto size = tg::vec2i(96, 96);
    auto const forward
        = co_await draw_hybrid(ctx, resources, item, cam, lights, size, sv::hybrid_lighting::forward, 4, sky);
    auto const deferred
        = co_await draw_hybrid(ctx, resources, item, cam, lights, size, sv::hybrid_lighting::deferred, 4, sky);

    auto floor = 0;
    auto off = 0;
    auto worst = 0.0f;
    for (auto i = isize(0); i < forward.color.size(); ++i)
    {
        auto const n = forward.normal[i];
        if (f32(n.r) == 0.0f && f32(n.g) == 0.0f && f32(n.b) == 0.0f)
            continue;
        ++floor;
        auto const lum = [](tg::vec4f c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
        auto const f = lum(forward.color[i]);
        auto const d = lum(deferred.color[i]);
        auto const miss = tg::abs(d - f) / cc::max(f, 1e-3f);
        worst = cc::max(worst, miss);
        if (miss > 0.05f)
            ++off;
    }
    CC_LOG_INFO("a mirror floor near the origin: {} of {} pixels off by more than 5% deferred, the worst by {}", off,
                floor, worst);
    REQUIRE(floor > size[0] * size[1] / 4);
    CHECK(off == 0).dump("floor pixels", floor).dump("worst relative miss", worst);

    // The megakernel finds its hit along the ray as well, and continues from it the same way.
    // A sample leaving from under the floor meets its back, which darkens a quarter of the floor by 5 to 20%.
    // Its samples differ from the hybrid's, which leaves a few pixels 5% off either way, so only darker ones count.
    // It antialiases the floor's outline where the hybrid does not, so the pixels next to the sky are left out.
    auto const traced = co_await sv_test::trace_direct(ctx, resources, item, cam, lights, size, 4, 2, sky);
    auto const on_floor = [&](int x, int y)
    {
        if (x < 0 || y < 0 || x >= size[0] || y >= size[1])
            return false;
        auto const n = forward.normal[y * size[0] + x];
        return f32(n.r) != 0.0f || f32(n.g) != 0.0f || f32(n.b) != 0.0f;
    };
    auto inside = 0;
    auto traced_off = 0;
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            if (!on_floor(x, y) || !on_floor(x - 1, y) || !on_floor(x + 1, y) || !on_floor(x, y - 1)
                || !on_floor(x, y + 1))
                continue;
            ++inside;
            auto const lum = [](tg::vec4f c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
            auto const i = y * size[0] + x;
            if (lum(traced[i]) < 0.95f * lum(forward.color[i]))
                ++traced_off;
        }
    CC_LOG_INFO("a mirror floor near the origin: {} of {} inner pixels traced more than 5% darker", traced_off, inside);
    REQUIRE(inside > floor / 2);
    CHECK(traced_off < inside / 50).dump("inner floor pixels", inside).dump("darker", traced_off);
    co_await cc::async_settled(sv::background_work(ctx));
}

// The studio floor, two triangles a thousand units across, seen from a camera a unit above it, is its albedo times a uniform sky everywhere.
// A traversal's `t` is rounded to the triangle's corners' distance from the ray's origin, far more than the short ray's own length.
// Lifted only by the ray's rounding, a share of each bounce started under the floor, and the floor shadowed itself in streaks.
ASYNC_INVOCABLE_TEST("sv::hybrid_routine - a short ray onto the thousand-unit studio floor leaves it from above, "
                     "traced, "
                     "forward and deferred",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    sv_test::shared_material_library();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(sv::studio_floor());
    resources.wait_for_pending_uploads();

    auto cam = sv::camera::looking_at(tg::pos3d(0, 1, 0), tg::pos3d(0, 0, 4));
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(60.0);
    auto const lights = sv_test::light_table_of(
        {.center = tg::vec3f(0, 50, 0), .half_x = 0.1f, .half_z = 0.1f, .emission = tg::vec3f(0, 0, 0)});
    auto const sky = 0.5f;
    auto const background = sv::background::uniform(tg::vec3f(sky, sky, sky));
    auto const expected = sv::studio_floor_albedo * sky;

    constexpr auto size = tg::vec2i(96, 96);
    // The bottom third is floor wherever the camera looks, well short of the horizon.
    auto const first_row = size[1] * 2 / 3;
    auto const pixels = size[0] * (size[1] - first_row);
    auto const lum = [](tg::vec4f c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };

    // The megakernel's four frames leave a pixel within a few percent, so every pixel is held to 10% as well as the mean.
    auto const traced = co_await sv_test::trace_direct(ctx, resources, item, cam, lights, size, 4, 2, background);
    auto darker = 0;
    auto traced_sum = 0.0f;
    for (auto y = first_row; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            auto const l = lum(traced[y * size[0] + x]);
            traced_sum += l;
            if (l < 0.9f * expected)
                ++darker;
        }
    auto const traced_mean = traced_sum / f32(pixels);
    CC_LOG_INFO("a short ray onto the studio floor: traced {} against {}, {} of {} pixels more than 10% darker",
                traced_mean, expected, darker, pixels);
    CHECK(darker == 0);
    CHECK(traced_mean > 0.98f * expected).dump("mean", traced_mean);

    // The hybrid's one light sample a frame leaves its pixels noisy, so it is held to its mean, which the streaks took 8% off.
    for (auto const lighting : {sv::hybrid_lighting::forward, sv::hybrid_lighting::deferred})
    {
        auto const name = lighting == sv::hybrid_lighting::forward ? "forward" : "deferred";
        auto const hybrid = co_await draw_hybrid(ctx, resources, item, cam, lights, size, lighting, 4, background);
        auto sum = 0.0f;
        for (auto y = first_row; y < size[1]; ++y)
            for (auto x = 0; x < size[0]; ++x)
                sum += lum(hybrid.color[y * size[0] + x]);
        auto const mean = sum / f32(pixels);
        CC_LOG_INFO("a short ray onto the studio floor: {} {} against {}", name, mean, expected);
        CHECK(mean > 0.98f * expected).context(name).dump("mean", mean);
        CHECK(mean < 1.02f * expected).context(name).dump("mean", mean);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

// The hybrid as a view's layer: each lighting accumulates its frames until a capture's sample target, as a traced layer does.
// The last case denoises too, through the guides the hybrid's G-buffer writes.
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
    struct case_of
    {
        sv::hybrid_lighting lighting = sv::hybrid_lighting::forward;
        sr::denoise_method denoiser = sr::denoise_method::none;
    };
    case_of const cases[] = {
        {.lighting = sv::hybrid_lighting::forward, .denoiser = sr::denoise_method::none},
        {.lighting = sv::hybrid_lighting::deferred, .denoiser = sr::denoise_method::none},
        {.lighting = sv::hybrid_lighting::deferred, .denoiser = sr::denoise_method::automatic},
    };
    for (auto const& c : cases)
    {
        auto const path
            = cc::format("{}/sv-hybrid-capture-{}-{}.png", cc::temp_directory_path(), int(c.lighting), int(c.denoiser));
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
            scene.settings(
                {.mode = sv::render_mode::hybrid, .hybrid_lighting = c.lighting, .reconstruct = {.denoiser = c.denoiser}});
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

namespace
{
/// A 1 x 1 quad level at height 0.3, under the Cornell box's rect light, wound both ways so a ray from either side meets it.
[[nodiscard]] sv::mesh veil_of(sv::material_id material)
{
    auto const a = tg::pos3f(-0.5f, 0.3f, -0.5f);
    auto const b = tg::pos3f(0.5f, 0.3f, -0.5f);
    auto const c = tg::pos3f(0.5f, 0.3f, 0.5f);
    auto const d = tg::pos3f(-0.5f, 0.3f, 0.5f);
    auto const positions = cc::vector<tg::pos3f>{a, b, c, a, c, d, a, c, b, a, d, c};
    return sv::mesh{.name = "veil",
                    .geometry = sv::triangle_geometry::create_from_positions(positions),
                    .material = material};
}

/// The mean of the hybrid's captured floor below the light, the box lit by its rect light alone, with `veil` placed where one is given.
cc::shared_async<f32> captured_floor(sg::context& ctx, cc::optional<sv::mesh> veil)
{
    constexpr auto size = tg::vec2i(96, 64);
    auto const path = cc::format("{}/sv-hybrid-veil.png", cc::temp_directory_path());
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    // Few frames: the shadow it checks for is the difference between 160 and 6 of 255.
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "16");
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "40");

    auto const box = shadowed_cornell_box();
    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/hybrid-veil"))
    {
        auto view = f.window().view();
        view.camera(camera);
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(box);
        if (veil.has_value())
            scene.add_mesh(veil.value());
        scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
        scene.settings({.mode = sv::render_mode::hybrid, .hybrid_lighting = sv::hybrid_lighting::deferred});
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

    // The floor at the box's center, below the veil and seen past its edge: a block of pixels around (48, 54).
    auto sum = 0.0f;
    auto count = 0;
    for (auto y = 52; y <= 56; ++y)
        for (auto x = 44; x <= 52; ++x)
            for (auto ch = 0; ch < 3; ++ch)
            {
                sum += f32(img.pixels[(y * img.width + x) * img.channels + ch]);
                ++count;
            }
    cc::remove_file(path);
    co_return sum / f32(count);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::hybrid - a decal that keeps coverage casts no shadow in the hybrid, and a thresholded cutout "
                     "does",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; "
         "issue: bug-sv-woa-inline-readback-fastfail");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    using binding = sv::material_attribute_binding;
    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    // glTF's BLEND, as the importer binds it: an opacity and no cutoff, so it stays a coverage fraction.
    auto blend = cc::vector<binding>();
    blend.push_back(binding::of("opacity", 0.5f));
    // glTF's MASK: the same value stepped against a cutoff, fully opaque here.
    auto mask = cc::vector<binding>();
    mask.push_back(binding::of("opacity", 1.0f));
    mask.push_back(binding::of("alpha_cutoff", 0.5f));
    auto const blend_material = lib.acquire(sv::material::create("veil blend", openpbr, cc::move(blend)));
    auto const mask_material = lib.acquire(sv::material::create("veil mask", openpbr, cc::move(mask)));

    auto const open = co_await captured_floor(ctx, {});
    auto const covered = co_await captured_floor(ctx, veil_of(blend_material));
    auto const masked = co_await captured_floor(ctx, veil_of(mask_material));
    CC_LOG_INFO("the floor below the light: {} open, {} under a coverage veil, {} under a thresholded one", open,
                covered, masked);

    // The hybrid's rays skip the coverage veil, so the floor is lit as with none; the thresholded one shadows it.
    CHECK(open > 40.0f);
    CHECK(tg::abs(covered - open) < 0.1f * open);
    CHECK(masked < 0.7f * open);
}

namespace
{
/// A hybrid layer denoised by `method` below native: moving frames scaled into the denoised slot, then still ones at full size.
cc::shared_async<cc::unit> moving_then_still(sg::context& ctx, sr::denoise_method method)
{
    CC_LOG_INFO("a moving hybrid layer, upscaled through {}", sr::to_string(method));
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(shadowed_cornell_box());
    resources.wait_for_pending_uploads();

    auto const output_size = tg::vec2i(64, 64);
    auto const moving_id = sv::view_id::from_string("moving");
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = moving_id;
        v.camera = sv_test::cornell_camera();
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.settings.mode = sv::render_mode::hybrid;
        scene.settings.hybrid_lighting = sv::hybrid_lighting::deferred;
        scene.settings.reconstruct.denoiser = method;
        scene.settings.reconstruct.scale = sr::render_scale_preset::performance;
        // A move ends on the first still frame, so what each frame draws does not depend on the clock.
        scene.settings.motion_hold_ms = 0.0f;
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
    }

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto store = sv::view_store{};
    auto const history = sv::view_history{};
    auto step = 0.0;
    auto const execute = [&](sg::command_list& cmd, u64 index, bool move)
    {
        if (move)
        {
            step += 0.05;
            def.views[0].camera.position = tg::pos3d(step, 0, -3.4);
        }
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index, history);
        return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                            output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    };
    auto const frame = [&](u64 index, bool move) -> cc::shared_async<cc::unit>
    {
        auto cmd = ctx.create_command_list();
        CHECK(execute(*cmd, index, move) == sg::routine_outcome::executed);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    };

    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba16_float);
    (void)co_await ctx.routines.idle_completion();
    REQUIRE(sv_test::frames_until_executed(ctx, [&](sg::command_list& cmd) { return execute(cmd, 0, true); }));
    auto const accumulation = [&]() -> sv::impl::temporal_slot const&
    { return *store.peek_ptr(moving_id)->temporal.get_ptr(sv::temporal_id::accumulation(0)); };

    // Until every permutation compiled, a stand-in counts as a changed image, so a still camera draws scaled too.
    auto warm_frames = 0;
    while (accumulation().accum_frame < 2)
    {
        (void)co_await frame(0, false);
        ++warm_frames;
        REQUIRE(warm_frames < 500);
    }

    for (auto i = u64(1); i <= 3; ++i)
    {
        (void)co_await frame(i, true);
        CHECK(accumulation().traced_scaled);
        CHECK(accumulation().accum_frame == 0);
    }
    auto const& frame_samples = *store.peek_ptr(moving_id)->temporal.get_ptr(sv::temporal_id::frame_samples(0));
    auto const input = frame_samples.scaled_denoise.extent();
    CHECK(input[0] < output_size[0]);
    CHECK(input[1] < output_size[1]);
    CHECK(store.peek_ptr(moving_id)->temporal.get_ptr(sv::temporal_id::denoised(0))->accum_frame == 1);

    (void)co_await frame(4, false);
    CHECK(!accumulation().traced_scaled);
    CHECK(accumulation().accum_frame == 1);
    (void)co_await frame(5, false);
    CHECK(accumulation().accum_frame == 2);
    co_await cc::async_settled(sv::background_work(ctx));
    co_return;
}
} // namespace

// A hybrid layer upscaling below native, as a traced one does: a moving frame is drawn at the upscaler's input extent,
// jittered, and upscaled into the denoised slot, and is no sample of the mean.
// The first frame the camera holds still restarts the mean at full size, and the next one accumulates onto it.
// Both ways a frame is upscaled: svgf with FSR behind it, and dlss_rr, which upscales by itself.
ASYNC_INVOCABLE_TEST("sv - a moving hybrid layer draws at the upscaler's extent and a still one at full size",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");
    auto const support = sr::query_reconstruct_support(ctx);
    auto const fsr_behind_svgf = support.fsr && support.svgf;
    if (!fsr_behind_svgf && !support.dlss_rr)
        SKIP("neither FSR behind svgf nor dlss_rr runs on this device");

    if (fsr_behind_svgf)
        co_await moving_then_still(ctx, sr::denoise_method::svgf);
    if (support.dlss_rr)
        co_await moving_then_still(ctx, sr::denoise_method::dlss_rr);
}

namespace
{
/// How a still hybrid layer reached its sample target: the frames it took, the samples it stopped at, and its mean.
struct packed_run
{
    int frames = 0;
    u32 samples = 0;
    cc::vector<tg::vec4f> mean;
};

/// The lit Cornell box drawn by a hybrid layer until `target` samples, packing up to `per_frame` into a still frame.
/// `warm` first draws it in a throwaway store until every permutation is built, so the run counts only its own frames.
cc::shared_async<packed_run> run_to_target(sg::context& ctx,
                                           sv::gpu_resource_manager& resources,
                                           sv::scene_item const& item,
                                           u32 target,
                                           u32 per_frame)
{
    auto const output_size = tg::vec2i(48, 32);
    auto const id = sv::view_id::from_string("packed");
    auto def = sv::viewer_definition{};
    def.sample_target = target;
    def.still_samples_per_frame = per_frame;
    {
        auto v = sv::view_data{};
        v.id = id;
        v.camera = sv_test::cornell_camera();
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.lights.push_back(
            {.id = sv::light_id::from_string("key"),
             .light = sv::light::rect(tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40)});
        scene.settings.mode = sv::render_mode::hybrid;
        scene.settings.hybrid_lighting = sv::hybrid_lighting::deferred;
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
    }

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto const history = sv::view_history{};
    auto index = u64(0);
    auto const execute = [&](sv::view_store& store, sg::command_list& cmd)
    {
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index++, history);
        return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                            output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    };
    auto const slot_in = [&](sv::view_store& store) -> sv::impl::temporal_slot const*
    {
        auto const* const rec = store.peek_ptr(id);
        return rec != nullptr ? rec->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
    };

    // Until every permutation compiled, a stand-in restarts the mean every frame, which would count against the run.
    {
        auto warm = sv::view_store{};
        REQUIRE(sv_test::frames_until_executed(ctx,
                                               [&](sg::command_list& cmd)
                                               {
                                                   (void)execute(warm, cmd);
                                                   auto const* const s = slot_in(warm);
                                                   return s != nullptr && s->accum_frame >= 2
                                                            ? sg::routine_outcome::executed
                                                            : sg::routine_outcome::declined;
                                               }));
    }

    auto store = sv::view_store{};
    auto run = packed_run{};
    while (slot_in(store) == nullptr || !slot_in(store)->is_at_target())
    {
        auto cmd = ctx.create_command_list();
        CHECK(execute(store, *cmd) == sg::routine_outcome::executed);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        ++run.frames;
        REQUIRE(run.frames <= int(target) + 1);
    }
    run.samples = slot_in(store)->samples;

    auto cmd = ctx.create_command_list();
    auto back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot_in(store)->texture.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_await ctx.idle_completion(); // an epoch advance drains the GPU but not the readback actor
    auto const pixels = back.try_get_data();
    REQUIRE(pixels.has_value());
    for (auto const p : pixels.value())
        run.mean.push_back(p);
    co_await cc::async_settled(sv::background_work(ctx));
    co_return run;
}
} // namespace

// A still hybrid frame packs several samples, each the one a frame of its own would draw, so a capture spends one frame's
// recording on many samples: most of a frame's cost in a scene of many instances.
// The pack changes how many frames the mean takes and nothing else — it stops on the same sample, at the same image.
ASYNC_INVOCABLE_TEST("sv - a still hybrid frame packs samples, and reaches the image one sample a frame would",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const item = resources.acquire_scene_item(shadowed_cornell_box());
    resources.wait_for_pending_uploads();

    // 13 is no multiple of 4, so the last pack is cut short at the target.
    auto const single = co_await run_to_target(ctx, resources, item, 13, 1);
    auto const packed = co_await run_to_target(ctx, resources, item, 13, 4);

    CHECK(single.frames == 13);
    // the first frame restarts the mean and draws one, then 4, 4 and the 4 that end on 13
    CHECK(packed.frames == 4);
    CHECK(single.samples == 13);
    CHECK(packed.samples == 13);

    REQUIRE(single.mean.size() == packed.mean.size());
    auto worst = 0.0f;
    auto lit = 0;
    for (auto i = isize(0); i < single.mean.size(); ++i)
    {
        for (auto c = 0; c < 3; ++c)
            worst = cc::max(worst, tg::abs(single.mean[i][c] - packed.mean[i][c]));
        if (single.mean[i][0] + single.mean[i][1] + single.mean[i][2] > 0.01f)
            ++lit;
    }
    REQUIRE(lit > int(single.mean.size()) / 4); // a black image would agree with anything
    CHECK(worst <= 1e-5f).dump("worst channel difference", worst);
}
