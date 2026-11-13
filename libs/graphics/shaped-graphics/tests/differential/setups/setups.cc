#include "../../pipeline/pipeline_harness.hh"
#include "../differential.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sg_test_differential_setups.hh>
#include <sgl_modules/diff_floats.hh>
#include <sgl_modules/diff_raster.hh>
#include <sgl_modules/diff_scene.hh>
#include <sgl_modules/diff_tallies.hh>
#include <sgl_modules/diff_targets.hh>
#include <sgl_modules/diff_texels.hh>
#include <sgl_modules/diff_words.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/capabilities.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/shader_asset.hh>

// The corpus's setups, each the C++ half of the SGL module of its name.
// Every input is generated rather than random, so every run on every machine reads the same ones.

using namespace cc::primitive_defines;

namespace
{
constexpr isize k_threads = 256;
constexpr isize k_outputs_per_thread = 4;
constexpr int k_extent = 16;
constexpr int k_levels = 5; // 16, 8, 4, 2 and 1

[[nodiscard]] u32 hashed(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb'352du;
    x ^= x >> 15;
    x *= 0x846c'a68bu;
    return x ^ (x >> 16);
}

[[nodiscard]] cc::vector<u32> word_inputs()
{
    auto words = cc::vector<u32>{0u, 1u, 0x8000'0000u, 0xffff'ffffu, 0x7fff'ffffu, 0x8000'0001u, 2u, 31u, 32u, 33u};
    for (auto i = words.size(); i < k_threads; ++i)
        words.push_back(hashed(u32(i)));
    return words;
}

/// -64 to 64, never zero, of every magnitude between: small fractions, whole numbers and values just off them.
[[nodiscard]] cc::vector<f32> float_inputs()
{
    auto floats = cc::vector<f32>();
    for (auto i = isize(0); i < k_threads; ++i)
    {
        auto const unit = f32(hashed(u32(i) + 0x9e37'79b9u) >> 8) / f32(1 << 24); // [0, 1)
        auto const magnitude = (i % 4 == 0) ? 64.0f * unit : (i % 4 == 1) ? unit : f32(i % 17) + 0.5f * unit;
        auto const value = cc::max(magnitude, 1.0f / 1024.0f);
        floats.push_back(i % 2 == 0 ? value : -value);
    }
    return floats;
}

[[nodiscard]] cc::vector<sg_diff::output> one_output(cc::string_view name, cc::span<byte const> bytes)
{
    auto result = cc::vector<sg_diff::output>();
    result.push_back({.name = cc::string(name), .bytes = cc::vector<byte>::create_copy_of(bytes)});
    return result;
}

/// The 16 by 16 rgba8_unorm texture of every texture-reading setup, each texel of each level its own hashed color.
/// Uploaded on a list of its own, so the list that reads it opens no barrier inside a scope.
[[nodiscard]] sg::texture_2d hashed_texture(sg::context& ctx)
{
    auto const texture
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = k_extent,
                                            .height = k_extent,
                                            .mip_levels = k_levels,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto cmd = ctx.create_command_list();
    for (auto level = 0; level < k_levels; ++level)
    {
        auto const side = k_extent >> level;
        auto texels = cc::vector<u32>();
        for (auto i = 0; i < side * side; ++i)
            texels.push_back(hashed(u32(level * 4096 + i)) | 0xff00'0000u);
        cmd->upload.bytes_to_texture(texture.raw(), cc::span<u32 const>(texels).as_bytes(), {.mip_level = level});
    }
    ctx.submit_command_list(cc::move(cmd));
    return texture;
}

template <class Group, class T>
cc::shared_async<cc::vector<sg_diff::output>> run_buffers(sg::context* ctx,
                                                          sg_diff::shaders_on_route on,
                                                          cc::vector<T> inputs)
{
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<Group>()});
    auto const input = ctx->persistent.create_buffer_from_data(inputs, sg::buffer_usage::readonly_buffer);
    auto const output
        = ctx->persistent.create_buffer_from_data(cc::vector<T>::create_filled(k_threads * k_outputs_per_thread, T(0)),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<Group>();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, Group{.input = input.as_readonly_buffer(), .output = output.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(output);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    co_return one_output("output", cc::span<T const>(data).as_bytes());
}

cc::shared_async<cc::vector<sg_diff::output>> run_texels(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_texels::io;
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<io>()});
    auto const source = hashed_texture(*ctx);
    auto const output = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(k_threads * k_outputs_per_thread),
        sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<io>();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, io{.source = source.as_texture_view(), .output = output.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(output);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    co_return one_output("output", cc::span<tg::vec4f const>(data).as_bytes());
}

cc::shared_async<cc::vector<sg_diff::output>> run_raster(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_raster::io;
    using target = sgl_modules::diff_raster::target;
    auto const vertex
        = co_await sg_diff::compiled_on(*ctx, on.route, *sg::test::differential_setups::fullscreen::fullscreen.asset);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = ctx->cached.acquire_pipeline_layout<io>(),
        .vertex_shader = vertex,
        .fragment_shader = on.entry,
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = target::states{.color = {.format = sg::pixel_format::rgba32_float}},
        .target_set = target::name,
    });
    auto const input = ctx->persistent.create_buffer_from_data(float_inputs(), sg::buffer_usage::readonly_buffer);
    auto const source = hashed_texture(*ctx);
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<io>(),
        io{.input = input.as_readonly_buffer(), .source = source.as_texture_view()});

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = k_extent, .height = k_extent, .colors = {sg::pixel_format::rgba32_float}, .target_set = target::name},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        });
    co_return one_output("color", pixels.colors[0].bytes);
}

/// A float of `mantissa_bits` and a 5-bit exponent, unsigned unless `has_sign`, as half and packed small floats store them.
[[nodiscard]] f32 small_float(u32 bits, int mantissa_bits, bool has_sign)
{
    auto const mantissa = bits & ((1u << mantissa_bits) - 1);
    auto const exponent = int((bits >> mantissa_bits) & 31u);
    auto const sign = has_sign && ((bits >> (mantissa_bits + 5)) & 1u) != 0 ? -1.0f : 1.0f;
    auto const scale = f32(1u << mantissa_bits);
    if (exponent == 0)
        return sign * f32(mantissa) / scale * (1.0f / 16384.0f);
    if (exponent == 31)
        return mantissa == 0 ? sign * cc::bit_cast<f32>(0x7f80'0000u) : cc::bit_cast<f32>(0x7fc0'0000u);
    auto const magnitude = (1.0f + f32(mantissa) / scale) * f32(1u << exponent) / 32768.0f;
    return sign * magnitude;
}

/// One target as floats, a channel each, decoded from its format; an integer target keeps its bits and compares exactly.
[[nodiscard]] sg_diff::output decoded(cc::string_view name, sg_test::target_pixels const& pixels)
{
    auto floats = cc::vector<f32>();
    auto const* const words = reinterpret_cast<u32 const*>(pixels.bytes.data());
    auto const texels = isize(pixels.width) * pixels.height;
    for (auto i = isize(0); i < texels; ++i)
        switch (pixels.format)
        {
        case sg::pixel_format::rgba8_unorm_srgb:
            for (auto c = 0; c < 4; ++c)
                floats.push_back(f32((words[i] >> (8 * c)) & 0xffu) / 255.0f);
            break;
        case sg::pixel_format::rgba16_float:
            for (auto c = 0; c < 4; ++c)
                floats.push_back(small_float((words[2 * i + c / 2] >> (16 * (c % 2))) & 0xffffu, 10, true));
            break;
        case sg::pixel_format::rgb10a2_unorm:
            for (auto c = 0; c < 3; ++c)
                floats.push_back(f32((words[i] >> (10 * c)) & 0x3ffu) / 1023.0f);
            floats.push_back(f32(words[i] >> 30) / 3.0f);
            break;
        case sg::pixel_format::rg11b10_float:
            floats.push_back(small_float(words[i] & 0x7ffu, 6, false));
            floats.push_back(small_float((words[i] >> 11) & 0x7ffu, 6, false));
            floats.push_back(small_float(words[i] >> 22, 5, false));
            break;
        default:
            break;
        }
    if (pixels.format == sg::pixel_format::r32_uint)
        return {.name = cc::string(name), .bytes = cc::vector<byte>::create_copy_of(pixels.bytes), .is_exact = true};
    return {.name = cc::string(name), .bytes = cc::vector<byte>::create_copy_of(cc::span<f32 const>(floats).as_bytes())};
}

cc::shared_async<cc::vector<sg_diff::output>> run_targets(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_targets::io;
    using target = sgl_modules::diff_targets::target;
    auto const vertex
        = co_await sg_diff::compiled_on(*ctx, on.route, *sg::test::differential_setups::layered::layered.asset);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = ctx->cached.acquire_pipeline_layout<io>(),
        .vertex_shader = vertex,
        .fragment_shader = on.entry,
        .rasterization = {.cull = sg::cull_mode::none},
        .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::less},
        .color_targets = target::states{.srgb = {.format = sg::pixel_format::rgba8_unorm_srgb},
                                        .hdr = {.format = sg::pixel_format::rgba16_float},
                                        .packed = {.format = sg::pixel_format::rgb10a2_unorm},
                                        .small = {.format = sg::pixel_format::rg11b10_float},
                                        .word = {.format = sg::pixel_format::r32_uint}},
        .depth_stencil_format = sg::pixel_format::depth32_float,
        .target_set = target::name,
    });
    auto const input = ctx->persistent.create_buffer_from_data(float_inputs(), sg::buffer_usage::readonly_buffer);
    auto const group = ctx->persistent.create_binding_group(ctx->cached.acquire_binding_group_layout<io>(),
                                                            io{.input = input.as_readonly_buffer()});

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = k_extent,
         .height = k_extent,
         .colors = {sg::pixel_format::rgba8_unorm_srgb, sg::pixel_format::rgba16_float, sg::pixel_format::rgb10a2_unorm,
                    sg::pixel_format::rg11b10_float, sg::pixel_format::r32_uint},
         .depth_stencil = sg::pixel_format::depth32_float,
         .target_set = target::name},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.draw({.vertex_range = {.offset = 0, .size = 6}});
        });
    auto result = cc::vector<sg_diff::output>();
    cc::string_view const names[] = {"srgb", "hdr", "packed", "small", "word"};
    for (auto i = 0; i < 5; ++i)
        result.push_back(decoded(names[i], pixels[i]));
    co_return result;
}

/// The `scene` setup's world, as diff_scene.sgl describes it.
struct scene_geometry
{
    cc::vector<f32> terrain_vertices; ///< 9 by 9 grid vertices, row after row
    cc::vector<u32> terrain_indices;  ///< geometry 0's triangles, then geometry 1's
    isize opaque_index_count = 0;     ///< geometry 0's share of `terrain_indices`
    cc::vector<f32> octahedron;       ///< eight triangles, not indexed
};

[[nodiscard]] scene_geometry scene_world()
{
    auto scene = scene_geometry();
    for (auto y = 0; y <= 8; ++y)
        for (auto x = 0; x <= 8; ++x)
        {
            scene.terrain_vertices.push_back(f32(x));
            scene.terrain_vertices.push_back(f32(y));
            scene.terrain_vertices.push_back(f32(hashed(u32(y * 9 + x) + 0x51ed'2701u) >> 8) / f32(1 << 24));
        }
    // geometry 0 is the cells left of x = 4, geometry 1 those right of it; the diagonal alternates cell by cell
    for (auto const is_right : {false, true})
    {
        for (auto cy = 0; cy < 8; ++cy)
            for (auto cx = is_right ? 4 : 0; cx < (is_right ? 8 : 4); ++cx)
            {
                auto const v00 = u32(cy * 9 + cx);
                auto const v10 = v00 + 1;
                auto const v01 = v00 + 9;
                auto const v11 = v00 + 10;
                u32 const cell[] = {v00, v10, v11, v00, v11, v01};
                u32 const flipped[] = {v00, v10, v01, v10, v11, v01};
                for (auto const v : (cx + cy) % 2 == 0 ? cell : flipped)
                    scene.terrain_indices.push_back(v);
            }
        if (!is_right)
            scene.opaque_index_count = scene.terrain_indices.size();
    }
    // each face (a, b, c) counterclockwise seen from outside
    constexpr f32 r = 0.6f;
    for (auto const sz : {1.0f, -1.0f})
        for (auto const sy : {1.0f, -1.0f})
            for (auto const sx : {1.0f, -1.0f})
            {
                f32 const a[] = {sx * r, 0, 0};
                f32 const b[] = {0, sy * r, 0};
                f32 const c[] = {0, 0, sz * r};
                auto const is_outward = sx * sy * sz > 0;
                for (auto const* const corner : {a, is_outward ? b : c, is_outward ? c : b})
                    for (auto k = 0; k < 3; ++k)
                        scene.octahedron.push_back(corner[k]);
            }
    return scene;
}

/// The world's structures, and the buffers they were built from, which live as long as they do.
struct built_scene
{
    scene_geometry world;
    sg::buffer<f32> terrain_vertices;
    sg::buffer<u32> terrain_indices;
    sg::buffer<f32> octahedron;
    sg::tlas_handle tlas;
};

/// Builds the world on a list of its own and submits it, so a later list meets a finished TLAS.
/// `software_trace` also writes it into the context's software pool, from the same data, which is what the interpreter reads.
[[nodiscard]] built_scene build_scene(sg::context& ctx, sg::accel_build_flags flags)
{
    auto scene = built_scene{.world = scene_world()};
    auto const& world = scene.world;
    scene.terrain_vertices
        = ctx.persistent.create_buffer_from_data(world.terrain_vertices, sg::buffer_usage::accel_structure_build_input);
    scene.terrain_indices
        = ctx.persistent.create_buffer_from_data(world.terrain_indices, sg::buffer_usage::accel_structure_build_input);
    scene.octahedron
        = ctx.persistent.create_buffer_from_data(world.octahedron, sg::buffer_usage::accel_structure_build_input);

    auto build = ctx.create_command_list();
    auto const vertex_count = world.terrain_vertices.size() / 3;
    auto const vertex_bytes = cc::span<f32 const>(world.terrain_vertices).as_bytes();
    auto const index_bytes = cc::span<u32 const>(world.terrain_indices).as_bytes();
    sg::blas_triangles const terrain[] = {
        {.vertices = scene.terrain_vertices.raw(),
         .vertex_count = vertex_count,
         .indices = scene.terrain_indices.raw(),
         .index_count = world.opaque_index_count,
         .cpu_vertices = vertex_bytes,
         .cpu_indices = index_bytes},
        {.vertices = scene.terrain_vertices.raw(),
         .vertex_count = vertex_count,
         .indices = scene.terrain_indices.raw(),
         .index_count = world.terrain_indices.size() - world.opaque_index_count,
         .index_offset_in_bytes = world.opaque_index_count * isize(sizeof(u32)),
         .is_opaque = false,
         .cpu_vertices = vertex_bytes,
         .cpu_indices = index_bytes},
    };
    auto const terrain_blas = build->raytracing.build_blas(terrain, flags);
    auto const shape = sg::blas_triangles{.vertices = scene.octahedron.raw(),
                                          .vertex_count = world.octahedron.size() / 3,
                                          .cpu_vertices = cc::span<f32 const>(world.octahedron).as_bytes()};
    auto const shape_blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&shape, 1), flags);
    sg::tlas_instance const instances[] = {
        {.blas = terrain_blas, .instance_id = 1},
        {.blas = shape_blas, .transform = {1, 0, 0, 2, 0, 1, 0, 2, 0, 0, 1, 2.5f}, .instance_id = 10},
        {.blas = shape_blas, .transform = {0, -1, 0, 6, 1, 0, 0, 2, 0, 0, 1, 2.5f}, .instance_id = 11},
        {.blas = shape_blas, .transform = {1.5f, 0, 0, 2, 0, 0.5f, 0, 6, 0, 0, 1, 2}, .instance_id = 12},
        {.blas = shape_blas, .transform = {-1, 0, 0, 6, 0, 1, 0, 6, 0, 0, 1, 2.5f}, .instance_id = 13},
    };
    scene.tlas = build->raytracing.build_tlas(instances, flags);
    ctx.submit_command_list(cc::move(build));
    return scene;
}

cc::shared_async<cc::vector<sg_diff::output>> run_scene(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_scene::io;
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<io>()});
    auto const scene = build_scene(*ctx, sg::accel_build_flag::fast_trace);
    auto const& tlas = scene.tlas;

    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(k_threads * 2), usage);
    auto const ids
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(k_threads * 2), usage);
    auto const layout = ctx->cached.acquire_binding_group_layout<io>();
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, io{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer(), .ids = ids.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const hits_back = cmd->download.data_from_buffer(hits);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_hits = co_await hits_back.data();
    auto const got_ids = co_await ids_back.data();
    auto result = one_output("hits", cc::span<tg::vec4f const>(got_hits).as_bytes());
    result.push_back(
        {.name = "ids", .bytes = cc::vector<byte>::create_copy_of(cc::span<tg::vec4f const>(got_ids).as_bytes())});
    co_return result;
}

/// The `tallies` setup's 64 totals as they start: the first 48 at 0, for add, max, or and xor, and the last 16 at all ones, for min and and.
[[nodiscard]] cc::vector<u32> starting_totals()
{
    auto totals = cc::vector<u32>::create_filled(64, 0u);
    for (auto i = 48; i < 64; ++i)
        totals[i] = 0xffff'ffffu;
    return totals;
}

cc::shared_async<cc::vector<sg_diff::output>> run_tallies(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_tallies::io;
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<io>()});
    auto const input = ctx->persistent.create_buffer_from_data(word_inputs(), sg::buffer_usage::readonly_buffer);
    auto const totals = ctx->persistent.create_buffer_from_data(
        starting_totals(), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<io>(),
        io{.input = input.as_readonly_buffer(), .totals = totals.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(totals);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    co_return one_output("totals", cc::span<u32 const>(data).as_bytes());
}

/// The tallies' half: the interpreter runs the threads one after another, which is one of the orders every total must be the same under.
sg_diff::interpreter_half tallies_for_interpreter(sg::context&)
{
    auto const start = starting_totals();
    auto const totals = cc::pinned_data<u32>::create_copy_of(cc::span<u32 const>(start)).as_mutable_bytes();
    auto const inputs = word_inputs();
    auto half = sg_diff::interpreter_half{.outputs = {{.name = "totals", .bytes = totals}}, .threads = k_threads};
    half.bindings.groups.push_back(
        {.name = "io",
         .members
         = {{.name = "input", .bytes = cc::pinned_data<u32>::create_copy_of(cc::span<u32 const>(inputs)).as_bytes()},
            {.name = "totals", .mutable_bytes = totals}}});
    return half;
}

/// The interpreter's half of a buffer setup: `inputs` as `io.input`, and a zeroed `io.output` the runs store into.
template <class T>
sg_diff::interpreter_half buffers_for_interpreter(cc::vector<T> const& inputs)
{
    auto const output = cc::pinned_data<T>::create_filled(k_threads * k_outputs_per_thread, T(0)).as_mutable_bytes();
    auto half = sg_diff::interpreter_half{.outputs = {{.name = "output", .bytes = output}}, .threads = k_threads};
    half.bindings.groups.push_back(
        {.name = "io",
         .members = {{.name = "input", .bytes = cc::pinned_data<T>::create_copy_of(cc::span<T const>(inputs)).as_bytes()},
                     {.name = "output", .mutable_bytes = output}}});
    return half;
}

/// `hashed_texture`'s texels as the floats a shader reads from rgba8_unorm, level after level.
sg_diff::interpreter_half texels_for_interpreter(sg::context&)
{
    auto texels = cc::vector<f32>();
    for (auto level = 0; level < k_levels; ++level)
    {
        auto const side = k_extent >> level;
        for (auto i = 0; i < side * side; ++i)
        {
            auto const packed = hashed(u32(level * 4096 + i)) | 0xff00'0000u;
            for (auto channel = 0; channel < 4; ++channel)
                texels.push_back(f32((packed >> (8 * channel)) & 0xffu) / 255.0f);
        }
    }
    auto const output
        = cc::pinned_data<f32>::create_filled(k_threads * k_outputs_per_thread * 4, 0.0f).as_mutable_bytes();
    auto half = sg_diff::interpreter_half{.outputs = {{.name = "output", .bytes = output}}, .threads = k_threads};
    auto source = sgl::check::texture_data{
        .width = k_extent,
        .height = k_extent,
        .levels = k_levels,
        .texels = cc::pinned_data<f32>::create_copy_of(cc::span<f32 const>(texels)).as_bytes(),
    };
    auto texture = sgl::check::member_data{.name = "source"};
    texture.textures.push_back(cc::move(source));
    auto group = sgl::check::bound_group{.name = "io"};
    group.members.push_back(cc::move(texture));
    group.members.push_back({.name = "output", .mutable_bytes = output});
    half.bindings.groups.push_back(cc::move(group));
    return half;
}

/// diff_raster.varyings at a pixel's center, as fullscreen.sgl writes them and a rasterizer interpolates them:
/// the position, then `uv`, which runs 0 to 1 across the target, then `weight`, linear in `uv`.
void raster_varyings(f32 x, f32 y, cc::span<f32> leaves)
{
    auto const u = x / f32(k_extent);
    auto const v = y / f32(k_extent);
    f32 const values[] = {x, y, 0.0f, 1.0f, u, v, u * 3.0f - v * 0.5f + 0.25f};
    for (auto i = isize(0); i < leaves.size() && i < isize(sizeof(values) / sizeof(f32)); ++i)
        leaves[i] = values[i];
}

/// The raster setup's half: the floats input and the texels texture, and a 16 by 16 target of interpolated varyings.
sg_diff::interpreter_half raster_for_interpreter(sg::context& ctx)
{
    auto half = texels_for_interpreter(ctx);
    auto& io = half.bindings.groups[0];
    io.members.remove_back(); // the texels setup's output, which a pixel stage writes as its target instead
    auto const inputs = float_inputs();
    io.members.push_back(
        {.name = "input", .bytes = cc::pinned_data<f32>::create_copy_of(cc::span<f32 const>(inputs)).as_bytes()});
    half.outputs = {{.name = "color"}};
    half.threads = 0;
    half.extent = k_extent;
    half.pixel_varyings = &raster_varyings;
    return half;
}

/// The scene's half: the world built into the context's software pool as well, which the interpreter traces as sg's software walk does.
/// The pool's words are copied out once the build has written them, which it does as it is recorded.
sg_diff::interpreter_half scene_for_interpreter(sg::context& ctx)
{
    auto half = sg_diff::interpreter_half{.threads = k_threads};
    auto* const pool = ctx.software_pool();
    if (!ctx.supports(sg::feature::ray_query) || pool == nullptr)
    {
        half.skipped = "this context builds no software trace for the interpreter to read";
        return half;
    }
    auto const scene = build_scene(ctx, sg::accel_build_flag::fast_trace | sg::accel_build_flag::software_trace);
    auto const storage = pool->storage();
    half.bindings.acceleration_pool
        = cc::pinned_data<byte>::create_copy_of(cc::span<byte const>(storage.mapped, storage.size_in_bytes));

    for (auto const* const name : {"hits", "ids"})
        half.outputs.push_back(
            {.name = name, .bytes = cc::pinned_data<tg::vec4f>::create_defaulted(k_threads * 2).as_mutable_bytes()});
    half.bindings.groups.push_back(
        {.name = "io",
         .members = {{.name = "world", .acceleration_root = sg::impl::software_region_of(*scene.tlas).unit},
                     {.name = "hits", .mutable_bytes = half.outputs[0].bytes},
                     {.name = "ids", .mutable_bytes = half.outputs[1].bytes}}});
    return half;
}

bool const words_registered = sg_diff::register_setup({
    .name = "words",
    .run = [](sg::context* ctx, sg_diff::shaders_on_route on)
    { return run_buffers<sgl_modules::diff_words::io>(ctx, cc::move(on), word_inputs()); },
    .routes = {},
    .across = {},
    .interpreted = [](sg::context&) { return buffers_for_interpreter(word_inputs()); },
});

bool const floats_registered = sg_diff::register_setup({
    .name = "floats",
    .run = [](sg::context* ctx, sg_diff::shaders_on_route on)
    { return run_buffers<sgl_modules::diff_floats::io>(ctx, cc::move(on), float_inputs()); },
    .routes = {.kind = sg_diff::tolerance::measure::ulps, .amount = 2},
    .across = {.kind = sg_diff::tolerance::measure::ulps, .amount = 16},
    .interpreted = [](sg::context&) { return buffers_for_interpreter(float_inputs()); },
});

// A filtered sample's weights carry as few as 8 fractional bits on some hardware, so across APIs a texel's difference is bounded by its 256th.
bool const texels_registered = sg_diff::register_setup({
    .name = "texels",
    .run = &run_texels,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
    .interpreted = &texels_for_interpreter,
});

// One driver traverses both routes' traces, so they agree to the bit; two implementations intersect to within rounding.
bool const scene_registered = sg_diff::register_setup({
    .name = "scene",
    .required = sg::feature::ray_query,
    .run = &run_scene,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.0001},
    .interpreted = &scene_for_interpreter,
});

cc::shared_async<cc::vector<sg_diff::output>> run_blended(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_targets::io;
    using target = sgl_modules::diff_targets::blend_target;
    constexpr auto add = sg::blend_component{.source = sg::blend_factor::one, .target = sg::blend_factor::one};
    auto const vertex
        = co_await sg_diff::compiled_on(*ctx, on.route, *sg::test::differential_setups::layered::layered.asset);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = ctx->cached.acquire_pipeline_layout<io>(),
        .vertex_shader = vertex,
        .fragment_shader = on.entry,
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets
        = target::states{.over = {.format = sg::pixel_format::rgba8_unorm_srgb, .blend = sg::blend_alpha},
                         .sum = {.format = sg::pixel_format::rgba16_float,
                                 .blend = sg::blend_state{.color = add, .alpha = add}},
                         .darkest
                         = {.format = sg::pixel_format::rgb10a2_unorm,
                            .blend = sg::blend_state{.color = {.target = sg::blend_factor::one, .op = sg::blend_op::min},
                                                     .alpha = {.target = sg::blend_factor::one, .op = sg::blend_op::min}}},
                         .difference = {.format = sg::pixel_format::rgba16_float,
                                        .blend = sg::blend_state{.color = {.target = sg::blend_factor::one,
                                                                           .op = sg::blend_op::subtract},
                                                                 .alpha = add}}},
        .target_set = target::name,
    });
    auto const input = ctx->persistent.create_buffer_from_data(float_inputs(), sg::buffer_usage::readonly_buffer);
    auto const group = ctx->persistent.create_binding_group(ctx->cached.acquire_binding_group_layout<io>(),
                                                            io{.input = input.as_readonly_buffer()});

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = k_extent,
         .height = k_extent,
         .colors = {sg::pixel_format::rgba8_unorm_srgb, sg::pixel_format::rgba16_float, sg::pixel_format::rgb10a2_unorm,
                    sg::pixel_format::rgba16_float},
         .target_set = target::name,
         .clear_color = tg::vec4f(0.2f, 0.4f, 0.6f, 0.5f)},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.draw({.vertex_range = {.offset = 0, .size = 6}});
        });
    auto result = cc::vector<sg_diff::output>();
    cc::string_view const names[] = {"over", "sum", "darkest", "difference"};
    for (auto i = 0; i < 4; ++i)
        result.push_back(decoded(names[i], pixels[i]));
    co_return result;
}

cc::shared_async<cc::vector<sg_diff::output>> run_samples(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_targets::io;
    using samples = sgl_modules::diff_targets::samples;
    using target = sgl_modules::diff_targets::sampled_target;
    constexpr int sample_count = 4;
    auto const vertex
        = co_await sg_diff::compiled_on(*ctx, on.route, *sg::test::differential_setups::layered::layered.asset);
    auto const read_stage = co_await sg_diff::compiled_on(
        *ctx, on.route, *sg::test::differential_setups::read_samples::read_samples.asset);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = ctx->cached.acquire_pipeline_layout<io>(),
        .vertex_shader = vertex,
        .fragment_shader = on.entry,
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = target::states{.color = {.format = sg::pixel_format::rgba16_float}},
        .sample_count = sample_count,
        .target_set = target::name,
    });
    auto const read = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = read_stage, .layout = ctx->cached.acquire_pipeline_layout<samples>()});

    auto const input = ctx->persistent.create_buffer_from_data(float_inputs(), sg::buffer_usage::readonly_buffer);
    auto const group = ctx->persistent.create_binding_group(ctx->cached.acquire_binding_group_layout<io>(),
                                                            io{.input = input.as_readonly_buffer()});
    auto const drawn
        = ctx->persistent.create_texture_2d_ms({.format = sg::pixel_format::rgba16_float,
                                                .width = k_extent,
                                                .height = k_extent,
                                                .sample_count = sample_count,
                                                .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(k_extent * k_extent * sample_count),
        sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    {
        auto scope = cmd->raster.render_to(
            {.color_targets = {drawn.as_render_target_view().cleared(tg::vec4f(0.2f, 0.4f, 0.6f, 0.5f))},
             .target_set = target::name});
        scope.bind_pipeline(*pipeline);
        scope.bind_group(0, *group);
        scope.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const read_group = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<samples>(),
        samples{.target = drawn.as_texture_view(), .values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*read);
    cmd->compute.bind_group(0, *read_group);
    cmd->compute.dispatch_threads(k_extent * k_extent);
    auto const back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    co_return one_output("samples", cc::span<tg::vec4f const>(got).as_bytes());
}

// One driver encodes both routes' writes alike; two implementations round a float into a format each their own way, by a step of it.
bool const targets_registered = sg_diff::register_setup({
    .name = "targets",
    .stage = sg::shader_stage::fragment,
    .run = &run_targets,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
});

// Blending into an sRGB target runs in linear space, and an implementation rounds the encode either way, so the bound is the targets' step.
bool const blended_registered = sg_diff::register_setup({
    .name = "blended",
    .stage = sg::shader_stage::fragment,
    .module = "diff_targets",
    .run = &run_blended,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
});

// Coverage at the standard sample positions is the same on every device; a sample's value is a half float, written alike everywhere.
bool const samples_registered = sg_diff::register_setup({
    .name = "samples",
    .stage = sg::shader_stage::fragment,
    .module = "diff_targets",
    .run = &run_samples,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
});

bool const tallies_registered = sg_diff::register_setup({
    .name = "tallies",
    .run = &run_tallies,
    .routes = {},
    .across = {},
    .interpreted = &tallies_for_interpreter,
});

bool const raster_registered = sg_diff::register_setup({
    .name = "raster",
    .stage = sg::shader_stage::fragment,
    .run = &run_raster,
    .routes = {.kind = sg_diff::tolerance::measure::ulps, .amount = 2},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
    .interpreted = &raster_for_interpreter,
});
} // namespace
