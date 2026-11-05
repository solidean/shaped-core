#include "viewer_test_env.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/hybrid_routine.hh>
#include <shaped-viewer/rendering/raster_routine.hh>

using namespace cc::primitive_defines;

// What the raster path's culling and depth prepass save on Intel's Sponza: the same frames drawn with no culling, culled by the view, and culled by occlusion too.
// The last two again with the opaque depth laid down first, so each pixel is shaded once.
// Then the hybrid both ways, forward and deferred, culled by occlusion after a prepass: direct light with its shadow rays traced inline.
//
// GPU time rather than wall time: each frame is bracketed by its own timestamps.
// The contenders run interleaved frame by frame, since a laptop GPU's clocks drift under sustained load and a later run would come out slower.
// The camera walks along the nave, so the view culling has something out of view and the occlusion culling pillars and walls to hide behind.
// A benchmark rather than a test: it asserts nothing, and libs/graphics/shaped-viewer/docs/raster.md records what it measured.

namespace
{
constexpr auto k_size = tg::vec2i(1920, 1080);
constexpr int k_frames = 48;

/// The GPU seconds between each pair of timestamps, once all of them have landed.
[[nodiscard]] f64 seconds_of(cc::span<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp> const> spans)
{
    auto seconds = 0.0;
    for (auto const& [from, to] : spans)
        seconds += to.try_get_seconds().value() - from.try_get_seconds().value();
    return seconds;
}

[[nodiscard]] sg::texture_2d target_of(sg::context& ctx, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d(
        {.format = format,
         .width = k_size[0],
         .height = k_size[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::render_target | sg::texture_usage::image});
}

/// `t` as a TLAS instance's row-major 3x4, which is what places an instance record.
void pack_transform(sg::tlas_instance& inst, tg::affine_transform3f const& t)
{
    auto const l = t.linear_mat();
    auto const p = t.translation();
    for (auto r = 0; r < 3; ++r)
    {
        for (auto c = 0; c < 3; ++c)
            inst.transform[r * 4 + c] = l[c, r];
        inst.transform[r * 4 + 3] = p[r];
    }
}
} // namespace

namespace
{
/// Draws `asset` along the longer horizontal axis of its box, at `eye_height` above its floor, every contender's way, and logs each.
/// `eye_height` is in the asset's own units, or where it is none 5% of the box's height, for an asset whose unit is unknown.
cc::shared_async<cc::unit> measure(sg::context& ctx,
                                   cc::string_view name,
                                   sv::asset_data const& asset,
                                   cc::optional<f64> eye_height)
{
    auto const box = asset.bounds();
    REQUIRE(box.has_value());

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto items = cc::vector<sv::scene_item>();
    auto transforms = cc::vector<tg::affine_transform3f>();
    for (auto const& mesh : asset.meshes)
    {
        items.push_back(resources.acquire_scene_item(mesh));
        transforms.push_back(mesh.transform);
    }
    resources.wait_for_pending_uploads();
    // The hybrid traces against them; the raster frames never read them.
    auto blases = cc::vector<sg::blas_handle>();
    for (auto const& item : items)
        blases.push_back(sv_test::blas_of(resources, item.mesh));

    // One permutation per distinct shader key, and each draw's row is its item's position.
    auto permutations = cc::vector<sv::material_permutation const*>();
    auto draws = cc::vector<sv::raster_draw>();
    for (auto i = isize(0); i < items.size(); ++i)
    {
        auto const* const permutation = resources.shaders.find(items[i].shader_key);
        REQUIRE(permutation != nullptr);
        auto index = isize(0);
        while (index < permutations.size() && permutations[index] != permutation)
            ++index;
        if (index == permutations.size())
            permutations.push_back(permutation);
        auto const* const mesh_rec = resources.meshes.get_ptr(items[i].mesh);
        REQUIRE(mesh_rec != nullptr);
        draws.push_back({.instance = u32(i),
                         .triangle_count = u32(mesh_rec->geometry.triangle_count),
                         .permutation = u32(index),
                         .first_index = u32(mesh_rec->geometry.first_index),
                         .base_vertex = u32(mesh_rec->geometry.base_vertex),
                         .bounds = mesh_rec->bounds});
    }
    CC_LOG_INFO("{}: {} draws over {} permutations", name, draws.size(), permutations.size());

    auto const targets = sv::raster_gbuffer{.color = target_of(ctx, sg::pixel_format::rgba32_float),
                                            .normal = target_of(ctx, sg::pixel_format::rgba16_float),
                                            .depth = target_of(ctx, sg::pixel_format::r32_float),
                                            .albedo = target_of(ctx, sg::pixel_format::rgba16_float),
                                            .specular_albedo = target_of(ctx, sg::pixel_format::rgba16_float),
                                            .roughness = target_of(ctx, sg::pixel_format::r16_float),
                                            .motion = target_of(ctx, sg::pixel_format::rg32_float),
                                            .primary_depth = target_of(ctx, sg::pixel_format::r32_float)};

    // At head height, walking along the box's longer horizontal axis from one end toward the other.
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

    struct contender
    {
        cc::string_view name;
        sv::raster_culling culling = sv::raster_culling::none;
        bool depth_prepass = false;
        /// Where set, the frame is the hybrid's, lit this way.
        cc::optional<sv::hybrid_lighting> hybrid;
    };
    contender const contenders[] = {
        {.name = "no culling", .culling = sv::raster_culling::none, .depth_prepass = false},
        {.name = "view culling", .culling = sv::raster_culling::view, .depth_prepass = false},
        {.name = "occlusion culling", .culling = sv::raster_culling::occlusion, .depth_prepass = false},
        {.name = "view culling + prepass", .culling = sv::raster_culling::view, .depth_prepass = true},
        {.name = "occlusion culling + prepass", .culling = sv::raster_culling::occlusion, .depth_prepass = true},
        {.name = "hybrid forward",
         .culling = sv::raster_culling::occlusion,
         .depth_prepass = true,
         .hybrid = sv::hybrid_lighting::forward},
        {.name = "hybrid deferred",
         .culling = sv::raster_culling::occlusion,
         .depth_prepass = true,
         .hybrid = sv::hybrid_lighting::deferred},
    };
    auto const count = cc::span<contender const>(contenders).size();
    // One record per occlusion contender, as two layers would keep them.
    auto visibility = cc::vector<sg::buffer<u32>>::create_defaulted(count);

    // One frame of `c`: true where it executed with every permutation's own shader bound.
    auto const record = [&](sg::command_list& cmd, isize c, int frame) -> bool
    {
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        auto instances = cc::vector<sg::tlas_instance>();
        for (auto i = isize(0); i < items.size(); ++i)
        {
            records.push_back(resources.describe_instance(cmd, items[i].mesh, items[i].instance));
            auto inst = sg::tlas_instance{.blas = blases[i], .instance_id = u32(i), .hit_group_offset = 0};
            pack_transform(inst, transforms[i]);
            sv::place_instance_record(records.back(), inst, permutations[draws[i].permutation]->opacity,
                                      permutations[draws[i].permutation]->secondary);
            instances.push_back(inst);
        }
        auto const cam = camera_at(frame);
        auto const camera = sv::camera_record_of(cam);
        auto const background = ctx.transient.create_buffer_from_pod(
            cmd, sv::background_gpu::from(sv::background::studio()), sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);
        auto const bindless = resources.freeze();
        auto stand_ins = sv::stand_in_report{};
        if (contenders[c].hybrid.has_value())
        {
            auto fc = sv::default_frame_constants();
            fc.camera = camera;
            fc.previous_camera = camera;
            fc.near_plane = f32(cam.projection.near_plane);
            fc.pixel_spread = 2.0f * camera.up_scaled.length() / f32(k_size[1]);
            fc.rng_seed = u32(frame) + 1u;
            auto const outcome = sv::hybrid_routine::execute(cmd, {.lighting = contenders[c].hybrid.value(),
                                                                   .frame = fc,
                                                                   .background = background,
                                                                   .instances = instances,
                                                                   .instance_table = instance_table,
                                                                   .draws = draws,
                                                                   .index_pool = resources.meshes.index_pool(),
                                                                   .permutations = permutations,
                                                                   .fallback = &resources.shaders.acquire_fallback(),
                                                                   .bindless = &bindless,
                                                                   .targets = targets,
                                                                   .stand_ins = &stand_ins,
                                                                   .visibility = &visibility[c],
                                                                   .culling = contenders[c].culling});
            return outcome == sg::routine_outcome::executed && stand_ins.hash == 0;
        }
        auto const outcome = sv::raster_routine::execute(
            cmd, {.frame = {.camera = camera,
                            .previous_camera = camera,
                            .near_plane = f32(cam.projection.near_plane),
                            .light_count = 0,
                            .decal_count = 0,
                            .pixel_spread = 2.0f * camera.up_scaled.length() / f32(k_size[1]),
                            .size = tg::vec2f(f32(k_size[0]), f32(k_size[1])),
                            .jitter = tg::vec2f(0, 0)},
                  .background = background,
                  .instance_table = instance_table,
                  .draws = draws,
                  .index_pool = resources.meshes.index_pool(),
                  .permutations = permutations,
                  .fallback = &resources.shaders.acquire_fallback(),
                  .bindless = &bindless,
                  .targets = targets,
                  .stand_ins = &stand_ins,
                  .visibility = &visibility[c],
                  .culling = contenders[c].culling,
                  .depth_prepass = contenders[c].depth_prepass});
        return outcome == sg::routine_outcome::executed && stand_ins.hash == 0;
    };

    // Until every permutation compiled and every texture's follow-up work drained, so every contender times the same work.
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        (void)resources.record_pending_work(*cmd);
        auto ready = true;
        for (auto c = isize(0); c < count; ++c)
            ready = record(*cmd, c, 0) && ready;
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (ready && resources.pending_work_count() == 0)
            break;
        REQUIRE(cc::current_time_steady_secs() - start < 600.0);
        sv_test::drive_ambient_work();
    }

    auto spans = cc::vector<cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>>();
    for (auto c = isize(0); c < count; ++c)
        spans.push_back({});
    for (auto f = 0; f < k_frames; ++f)
        for (auto c = isize(0); c < count; ++c)
        {
            auto cmd = ctx.create_command_list();
            auto const from = cmd->query.record_gpu_timestamp();
            auto const executed = record(*cmd, c, f);
            auto const to = cmd->query.record_gpu_timestamp();
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            REQUIRE(executed);
            spans[c].push_back({from, to});
        }
    co_await ctx.idle_completion();

    for (auto c = isize(0); c < count; ++c)
        CC_LOG_INFO("{} {}x{}: {} {} ms per frame", name, k_size[0], k_size[1], contenders[c].name,
                    1e3 * seconds_of(spans[c]) / f64(k_frames));

    co_await cc::async_settled(sv::background_work(ctx));
    co_await ctx.idle_completion();
    co_return;
}
} // namespace

ASYNC_BENCHMARK("sv - raster on Sponza, culled by nothing, by the view and by occlusion, with and without a depth "
                "prepass")
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the raster shaders");
    CC_LOG_INFO("adapter: {}", created.value()->metrics.adapter().name);

    auto const loader = sv::asset_loader();
    auto const asset = loader.load(nx::source_path(SV_TEST_SPONZA_DIR "/NewSponza_Main_glTF_003.gltf"));
    if (asset.has_error())
        SKIP(cc::format("Sponza did not load ({}); uv run extern/sponza/fetch-sponza.py installs it",
                        asset.error().to_string()));
    co_await measure(*created.value(), "sponza", asset.value(), 1.7);
}

// Many small pieces in a street of buildings, which is where occlusion culling has the most to hide.
ASYNC_BENCHMARK("sv - raster on Bistro, culled by nothing, by the view and by occlusion, with and without a depth "
                "prepass")
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the raster shaders");
    CC_LOG_INFO("adapter: {}", created.value()->metrics.adapter().name);

    auto const loader = sv::asset_loader();
    auto const asset = loader.load(nx::source_path(SV_TEST_BISTRO_DIR "/BistroExterior.fbx"));
    if (asset.has_error())
        SKIP(cc::format("Bistro did not load ({}); uv run extern/bistro/fetch-bistro.py installs it",
                        asset.error().to_string()));
    co_await measure(*created.value(), "bistro exterior", asset.value(), {});
}
