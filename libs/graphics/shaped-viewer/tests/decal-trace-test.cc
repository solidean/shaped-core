#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/context.hh> // sv::background_work
#include <shaped-viewer/drawing/decal.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>

using namespace cc::primitive_defines;

// Decals in the trace: a drawing projected onto a gray quad paints its base where the projector reaches and faces it.
// The quad lies at z = 0 facing +z, the camera looks at it from z = 3, and the environment is uniform white.

namespace
{
constexpr i32 image_size = 32;

/// The quad from (-1, -1) to (1, 1) at z = 0, wound so its geometric normal is +z, or -z where `flipped`.
[[nodiscard]] cc::vector<tg::pos3f> quad_positions(bool flipped = false)
{
    auto p = cc::vector<tg::pos3f>();
    for (auto const v : {tg::pos3f(-1, -1, 0), tg::pos3f(1, -1, 0), tg::pos3f(1, 1, 0)})
        p.push_back(v);
    for (auto const v : {tg::pos3f(-1, -1, 0), tg::pos3f(1, 1, 0), tg::pos3f(-1, 1, 0)})
        p.push_back(v);
    if (flipped)
        for (auto i = isize(0); i + 2 < p.size(); i += 3)
            cc::swap(p[i + 1], p[i + 2]);
    return p;
}

/// A red rectangle over `box`, by default a square 0.6 across centered on the drawing's origin.
[[nodiscard]] sv::drawing red_square(tg::aabb2f box = tg::aabb2f(tg::pos2f(-0.3f, -0.3f), tg::pos2f(0.3f, 0.3f)))
{
    auto d = sv::drawing();
    d.add_fill(sv::path::rectangle(box), {.color = tg::vec4f(1, 0, 0, 1)});
    return d;
}

/// The decal `d` resolved against `drawings`, as `scene_ref::add_decal` resolves it.
[[nodiscard]] sv::decal_placement placed(sv::drawing_manager& drawings, sv::drawing const& drawing, sv::decal const& d)
{
    auto const set = drawings.acquire_decal(drawing);
    return {.first_record = drawings.first_record(set, 0),
            .record_count = drawings.record_count(set, 0),
            .bounds = drawings.bounds(set, 0),
            .at = d.at,
            .x_axis = d.x_axis * d.scale,
            .y_axis = d.y_axis * d.scale,
            .depth = d.depth,
            .tint = sr::pack_rgba8(d.tint)};
}

/// Whether `px` is the red square rather than the gray quad.
[[nodiscard]] bool is_red(tg::vec4f const& px)
{
    return px[0] > 0.05f && px[0] > 4.0f * px[1] && px[0] > 4.0f * px[2];
}

/// Whether `px` is the gray quad: lit, and neutral.
[[nodiscard]] bool is_gray(tg::vec4f const& px)
{
    return px[1] > 0.05f && tg::abs(px[0] - px[1]) < 0.25f * px[1];
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a decal paints the traced surface its projector reaches and faces",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const positions = quad_positions();
    sv_test::pbr_material const materials[] = {{.roughness = 1.0f}, {.roughness = 1.0f}};
    auto const quad = resources.acquire_scene_item(sv_test::as_mesh("quad", positions, materials));
    auto const flipped = resources.acquire_scene_item(sv_test::as_mesh("flipped quad", quad_positions(true), materials));
    resources.wait_for_pending_uploads();

    auto camera = sv::camera::looking_at(tg::pos3d(0, 0, 3), tg::pos3d::zero);
    camera.projection.aspect_ratio = 1.0;

    // Traces `item` under `decal` and reads the image back.
    auto const trace = [&](sv::decal const& decal, sv::scene_item const* item = nullptr,
                           sv::drawing const& drawing = red_square()) -> cc::shared_async<cc::vector<tg::vec4f>>
    {
        auto const& traced = item != nullptr ? *item : quad;
        auto const* const mesh_rec = resources.meshes.get_ptr(traced.mesh);
        REQUIRE(mesh_rec != nullptr);
        auto const* const permutation = resources.shaders.find(traced.shader_key);
        REQUIRE(permutation != nullptr);

        auto instances = cc::vector<sg::tlas_instance>();
        instances.push_back(sg::tlas_instance{.blas = mesh_rec->blas, .instance_id = 0, .hit_group_offset = 0});
        auto hit_groups = cc::vector<sv::material_permutation const*>();
        hit_groups.push_back(permutation);

        resources.drawings.begin_frame(ctx.current_epoch());
        auto const placement = placed(resources.drawings, drawing, decal);
        REQUIRE(placement.record_count == 1);

        auto shapes = cc::vector<sv::shaders::tracer::decal_shape>();
        auto& atlas = resources.drawings.decal_atlas();
        shapes.push_back(sv::impl::decal_shape_of(atlas.record(placement.first_record)));
        sv::shaders::tracer::decal_record const decals[] = {sv::impl::decal_record_of(placement, 0)};

        auto fc
            = sv::shaders::tracer::frame_constants{.camera = sv::camera_record_of(camera), .samples_per_pixel = 8, .max_bounces = 2, .rng_seed = 1};
        fc.decal_count = 1;
        fc.pixel_spread = 2.0f * fc.camera.up_scaled.length() / f32(image_size);

        auto pixels = cc::vector<tg::vec4f>();
        auto const loop_start = cc::current_time_steady_secs();
        while (pixels.empty())
        {
            (void)ctx.routines.tick();

            auto readback = sg::data_future<tg::vec4f>();
            auto cmd = ctx.create_command_list();
            atlas.prepare(*cmd);

            auto records = cc::vector<sv::shaders::tracer::instance_record>();
            records.push_back(resources.describe_instance(*cmd, traced.mesh, traced.instance));

            auto const frame = ctx.transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer);
            auto const background = ctx.transient.create_buffer_from_pod(
                *cmd, sv::background_gpu::from(sv::background::uniform(tg::vec3f(1, 1, 1))),
                sg::buffer_usage::readonly_buffer);
            auto const target = ctx.transient.create_texture_2d(
                {.format = sg::pixel_format::rgba32_float,
                 .width = image_size,
                 .height = image_size,
                 .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
            auto const instance_table
                = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
            auto const decal_buffer = ctx.transient.create_buffer_from_data(*cmd, cc::span<sv::shaders::tracer::decal_record const>(decals),
                                                                            sg::buffer_usage::readonly_buffer);
            auto const shape_buffer = ctx.transient.create_buffer_from_data(
                *cmd, cc::span<sv::shaders::tracer::decal_shape const>(shapes), sg::buffer_usage::readonly_buffer);

            auto const bindless = resources.freeze();
            auto const outcome = sv::pathtrace_routine::execute(*cmd, {.frame = frame,
                                                                       .background = background,
                                                                       .instances = instances,
                                                                       .output = target,
                                                                       .instance_table = instance_table,
                                                                       .hit_groups = hit_groups,
                                                                       .bindless = &bindless,
                                                                       .decals = decal_buffer,
                                                                       .decal_shapes = shape_buffer,
                                                                       .decal_atlas = &atlas});
            if (outcome == sg::routine_outcome::executed)
                readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(target.raw()));

            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();

            if (outcome != sg::routine_outcome::executed)
            {
                REQUIRE(cc::current_time_steady_secs() - loop_start < 45.0);
                sv_test::drive_ambient_work();
                continue;
            }

            REQUIRE(readback.is_valid());
            co_await ctx.idle_completion(); // an epoch advance drains the GPU but not the readback actor
            auto const delivered = readback.try_get_data();
            REQUIRE(delivered.has_value());
            for (auto const& px : delivered.value().span())
                pixels.push_back(px);
        }
        REQUIRE(pixels.size() == isize(image_size) * isize(image_size));
        co_return pixels;
    };

    auto const at = [](cc::span<tg::vec4f const> pixels, i32 x, i32 y) { return pixels[y * image_size + x]; };

    // One world unit on the quad is about 9 pixels here, so the square covers the center 5 or so, and 7 pixels off it is
    // still the quad.
    auto const center = image_size / 2;

    // Read the right way round from the camera, whose right is -x looking down -z: the projection runs along -z, into the quad.
    auto const facing
        = sv::decal{.at = tg::pos3f(0, 0, 0), .x_axis = tg::vec3f(-1, 0, 0), .y_axis = tg::vec3f(0, -1, 0), .depth = 0.5f};

    SECTION("inside the square the quad is red, and outside it gray")
    {
        auto const pixels = co_await trace(facing);
        CHECK(is_red(at(pixels, center, center)));
        CHECK(is_red(at(pixels, center - 1, center - 1)));
        CHECK(is_gray(at(pixels, center + 7, center)));
        CHECK(is_gray(at(pixels, center, center - 7)));
    }

    SECTION("it reads the right way round from the side it is projected from")
    {
        // Only the drawing's right half, which has to land on the camera's right.
        auto const right_half = red_square(tg::aabb2f(tg::pos2f(0, -0.3f), tg::pos2f(0.3f, 0.3f)));
        auto const pixels = co_await trace(facing, nullptr, right_half);
        CHECK(is_red(at(pixels, center + 1, center)));
        CHECK(is_gray(at(pixels, center - 2, center)));
    }

    SECTION("which way the mesh is wound does not matter, only which side faces the projector")
    {
        auto const pixels = co_await trace(facing, &flipped);
        CHECK(is_red(at(pixels, center, center)));
    }

    SECTION("a projector the surface faces away from paints nothing")
    {
        auto turned = facing;
        turned.y_axis = tg::vec3f(0, 1, 0);
        auto const pixels = co_await trace(turned);
        CHECK(is_gray(at(pixels, center, center)));
    }

    SECTION("a projector whose reach stops short of the surface paints nothing")
    {
        auto lifted = facing;
        lifted.at = tg::pos3f(0, 0, 1);
        auto const pixels = co_await trace(lifted);
        CHECK(is_gray(at(pixels, center, center)));
    }

    SECTION("the tint multiplies the drawing's color, its alpha included")
    {
        // Red times blue is black paint, which leaves the surface's specular alone.
        auto blue = facing;
        blue.tint = tg::vec4f(0, 0, 1, 1);
        auto const black = at(co_await trace(blue), center, center);
        CHECK(black[0] < 0.2f);
        CHECK(black[1] < 0.2f);

        auto clear = facing;
        clear.tint = tg::vec4f(1, 1, 1, 0);
        CHECK(is_gray(at(co_await trace(clear), center, center)));
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a decal coming, changing or going restarts the view's accumulation",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const positions = quad_positions();
    sv_test::pbr_material const materials[] = {{}, {}};
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("quad", positions, materials));
    resources.wait_for_pending_uploads();

    auto store = sv::view_store{};
    auto const view_with = [&](cc::span<sv::decal const> decals)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("decals");
        v.resolution = tg::vec2i(32, 32);
        v.camera = sv::camera::looking_at(tg::pos3d(0, 0, 3), tg::pos3d::zero);
        auto& layer = sv::ensure_scene_3d(v);
        layer.items.push_back(item);
        resources.drawings.begin_frame(ctx.current_epoch());
        for (auto const& d : decals)
            layer.decals.push_back(placed(resources.drawings, red_square(), d));
        return v;
    };

    // One frame that traced, through the view renderer's own path — which is what uploads the decals and binds the atlas.
    auto const trace = [&](sv::view_data const& view)
    {
        auto const before = store.accumulated_frames(view.id);
        auto const moved = sv_test::frames_until_executed(
            ctx,
            [&](sg::command_list& cmd)
            {
                resources.advance_to(ctx.current_epoch());
                store.begin_frame(u64(ctx.current_epoch()));
                (void)sv::view_renderer::execute(cmd, view, resources, store);
                return store.accumulated_frames(view.id) != before ? sg::routine_outcome::executed
                                                                   : sg::routine_outcome::declined;
            });
        REQUIRE(moved);
        return store.accumulated_frames(view.id);
    };

    auto const facing
        = sv::decal{.at = tg::pos3f(0, 0, 0), .x_axis = tg::vec3f(1, 0, 0), .y_axis = tg::vec3f(0, -1, 0), .depth = 0.5f};
    auto const none = cc::span<sv::decal const>();
    sv::decal const one[] = {facing};

    CHECK(trace(view_with(none)) == 1);
    CHECK(trace(view_with(none)) == 2);

    // Coming restarts, and an unchanged decal keeps counting.
    CHECK(trace(view_with(one)) == 1);
    CHECK(trace(view_with(one)) == 2);

    // Changing restarts.
    auto tinted = facing;
    tinted.tint = tg::vec4f(0, 1, 0, 1);
    sv::decal const changed[] = {tinted};
    CHECK(trace(view_with(changed)) == 1);

    // Going restarts; from 2, since `trace` tells a frame that traced by the count moving.
    CHECK(trace(view_with(changed)) == 2);
    CHECK(trace(view_with(none)) == 1);

    co_await cc::async_settled(sv::background_work(ctx));
}
