#pragma once

// What the raster and the hybrid tests share: the tracer's direct light they are both held to, and how its images are compared.

#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/half_float.hh>

namespace sv_test
{
using namespace cc::primitive_defines;

struct rgba16;
} // namespace sv_test

/// One `rgba16_float` texel.
struct sv_test::rgba16
{
    tg::f16 r = tg::f16(0.0f);
    tg::f16 g = tg::f16(0.0f);
    tg::f16 b = tg::f16(0.0f);
    tg::f16 a = tg::f16(0.0f);
};

namespace sv_test
{

/// The megakernel's direct light alone on `item` — the primary hit and its next-event estimate, nothing after — as the mean of `frames` frames.
/// `bounces` above 1 follows each path that many segments, a next-event estimate at every hit.
inline cc::shared_async<cc::vector<tg::vec4f>> trace_direct(sg::context& ctx,
                                                            sv::gpu_resource_manager& resources,
                                                            sv::scene_item const& item,
                                                            sv::camera const& cam,
                                                            sv::pt_light_table const& lights,
                                                            tg::vec2i size,
                                                            int frames,
                                                            int bounces = 1,
                                                            sv::background const& sky = {})
{
    auto const* const mesh_rec = resources.meshes.get_ptr(item.mesh);
    REQUIRE(mesh_rec != nullptr);
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = blas_of(resources, item.mesh), .instance_id = 0, .hit_group_offset = 0});
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
        fc.max_bounces = bounces;
        fc.rng_seed = u32(f) + 1u;
        fc.accum_frame = u32(f);

        auto cmd = ctx.create_command_list();
        auto records = cc::vector<sv::shaders::tracer::instance_record>();
        records.push_back(resources.describe_instance(*cmd, item.mesh, item.instance));
        sv::place_instance_record(records.back(), instances[0], permutation->opacity);

        auto const frame = ctx.transient.create_buffer_from_pod(*cmd, fc, sg::buffer_usage::readonly_buffer);
        auto const background = ctx.transient.create_buffer_from_pod(*cmd, sv::background_gpu::from(sky),
                                                                     sg::buffer_usage::readonly_buffer);
        auto const instance_table
            = ctx.transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = upload_lights(*cmd, lights);
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
            drive_ambient_work();
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
[[nodiscard]] inline bool is_interior(cc::span<rgba16 const> normals, tg::vec2i size, int x, int y)
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
[[nodiscard]] inline cc::optional<f64> block_mean(cc::span<tg::vec4f const> img,
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

/// Whether the raster path's shaders build here, which needs an SGL compiler and no ray tracing.
[[nodiscard]] inline bool can_raster()
{
    return shared_env().has_compiler;
}

/// Whether the device also runs the tracer a raster image is compared against.
[[nodiscard]] inline bool can_run(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported();
    ctx.drop_command_list(cc::move(probe));
    return supported && can_raster();
}

/// The camera the Cornell box tests look through: straight into the open side, square.
[[nodiscard]] inline sv::camera cornell_camera()
{
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    cam.projection.aspect_ratio = 1.0;
    return cam;
}
} // namespace sv_test
