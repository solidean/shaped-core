#include "probe.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/array.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <lod_probe_shaders.hh>
#include <shaped-graphics/all.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace lod_probe
{
namespace
{
using probe_vertex = shaders::lod_probe::probe_vertex;


/// Where a sweep starts; it covers two levels, so every pass crosses at least two halves away from either clamp.
constexpr float sweep_start = 3.0f;

/// The footprint shape of each row: ratio, texture-space angle, major axis along screen y.
struct row_shape
{
    float ratio;
    float angle_degrees;
    bool y_major;
};
constexpr row_shape row_shapes[tile_rows]
    = {{1, 0, false}, {2, 0, false}, {4, 45, false}, {8, 0, true}, {16, 0, false}, {16, 30, false}};

[[nodiscard]] f64 log2_of(f64 x)
{
    return tg::log(x) / tg::log(2.0);
}

[[nodiscard]] float exp2_of(float x)
{
    return float(tg::pow(2.0, f64(x)));
}

[[nodiscard]] float unclamped_lod(tile const& t, pass const& p)
{
    if (p.path == path::level)
        return t.minor_lod + p.sampler_bias;
    auto lod = specified_lod(t, p.anisotropy) + p.sampler_bias;
    if (p.path == path::shader_bias)
        lod += p.shader_bias;
    return lod;
}

/// Four corners per tile, each carrying the tile's gradients; the uv is an affine function of the pixel.
[[nodiscard]] cc::vector<probe_vertex> build_tile_vertices(cc::span<tile const> tiles, tg::vec2i target)
{
    auto out = cc::vector<probe_vertex>();
    for (auto i = 0; i < int(tiles.size()); ++i)
    {
        auto const f = footprint_of(tiles[i]);
        auto const origin = tg::vec2f(float((i % tile_columns) * tile_pixels), float((i / tile_columns) * tile_pixels));
        auto const s = float(pyramid_size);
        auto const grad_x = tg::vec2f(f.dx_u / s, f.dx_v / s);
        auto const grad_y = tg::vec2f(f.dy_u / s, f.dy_v / s);
        // Any offset works, since every mip is constant; a per-tile one keeps a stray fetch from looking systematic.
        auto const uv0 = tg::vec2f(0.37f * float(i), 0.11f * float(i));
        for (auto const corner : {tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(1, 1), tg::vec2f(0, 1)})
        {
            auto const pixel = origin + corner * float(tile_pixels);
            auto const local = corner * float(tile_pixels);
            out.push_back({
                .position
                = tg::vec2f(pixel[0] / float(target[0]) * 2.0f - 1.0f, 1.0f - pixel[1] / float(target[1]) * 2.0f),
                .uv = uv0 + grad_x * local[0] + grad_y * local[1],
                .grad_x = grad_x,
                .grad_y = grad_y,
                .level = tiles[i].minor_lod,
            });
        }
    }
    return out;
}

[[nodiscard]] cc::vector<u16> build_tile_indices(int tile_count)
{
    auto out = cc::vector<u16>();
    for (auto i = 0; i < tile_count; ++i)
        for (auto const k : {0, 1, 2, 0, 2, 3})
            out.push_back(u16(i * 4 + k));
    return out;
}

/// The mean of each tile's interior; the outer two pixels are left out so no quad straddles a tile edge.
[[nodiscard]] cc::vector<float> tile_means(cc::span<float const> pixels, tg::vec2i target, int tile_count)
{
    constexpr int border = 2;
    auto out = cc::vector<float>();
    for (auto i = 0; i < tile_count; ++i)
    {
        auto const x0 = (i % tile_columns) * tile_pixels;
        auto const y0 = (i / tile_columns) * tile_pixels;
        auto sum = 0.0;
        auto count = 0;
        for (auto y = y0 + border; y < y0 + tile_pixels - border; ++y)
            for (auto x = x0 + border; x < x0 + tile_pixels - border; ++x)
            {
                sum += pixels[y * target[0] + x];
                ++count;
            }
        out.push_back(float(sum / count));
    }
    return out;
}

} // namespace

[[nodiscard]] cc::any_error pipeline_error(cc::shared_async<sg::raster_pipeline_handle> const& building)
{
    auto const* const error = building->try_error();
    return cc::any_error(cc::format("the pipeline did not build: {}", error != nullptr ? error->underlying().to_string()
                                                                                       : cc::string("the build never "
                                                                                                    "ran")));
}

cc::string_view to_string(path p)
{
    switch (p)
    {
    case path::implicit:
        return "implicit";
    case path::shader_bias:
        return "shader-bias";
    case path::gradients:
        return "gradients";
    case path::level:
        return "level";
    }
    return "?";
}

footprint footprint_of(tile const& t)
{
    auto const minor = exp2_of(t.minor_lod);
    auto const major = minor * t.ratio;
    auto const rho_x = t.y_major ? minor : major;
    auto const rho_y = t.y_major ? major : minor;
    auto const [s, c] = tg::sin_cos(tg::angle_f::make_from_degree(t.angle_degrees));
    return {.dx_u = rho_x * c, .dx_v = rho_x * s, .dy_u = -rho_y * s, .dy_v = rho_y * c};
}

float specified_lod(tile const& t, int anisotropy)
{
    auto const f = footprint_of(t);
    auto const rho_x = tg::sqrt(f64(f.dx_u) * f.dx_u + f64(f.dx_v) * f.dx_v);
    auto const rho_y = tg::sqrt(f64(f.dy_u) * f.dy_u + f64(f.dy_v) * f.dy_v);
    auto const rho_max = cc::max(rho_x, rho_y);
    auto const rho_min = cc::min(rho_x, rho_y);
    auto const ratio = cc::min(rho_max / rho_min, f64(anisotropy));
    return float(log2_of(rho_max / ratio));
}

float expected_lod(tile const& t, pass const& p)
{
    return cc::clamp(unclamped_lod(t, p), 0.0f, float(pyramid_mips - 1));
}

bool is_clamped(tile const& t, pass const& p)
{
    auto const lod = unclamped_lod(t, p);
    return lod < 0.05f || lod > float(pyramid_mips - 1) - 0.05f;
}

cc::vector<tile> probe_tiles()
{
    auto out = cc::vector<tile>();
    for (auto const& shape : row_shapes)
        for (auto i = 0; i < tile_columns; ++i)
            out.push_back({
                .minor_lod = sweep_start + float(i) / float(sweep_steps_per_lod),
                .ratio = shape.ratio,
                .angle_degrees = shape.angle_degrees,
                .y_major = shape.y_major,
            });
    return out;
}

pass_summary summarize(pass_result const& r, cc::span<tile const> tiles, float ratio)
{
    auto offset_sum = 0.0;
    auto width_sum = 0.0;
    auto widths = 0;
    auto out = pass_summary();

    // The specified level where a row's sample first reaches `value`, interpolated between two columns; < 0 where it never does.
    auto const level_at = [&](int row, float value) -> float
    {
        for (auto c = 0; c + 1 < tile_columns; ++c)
        {
            auto const i = row * tile_columns + c;
            if (is_clamped(tiles[i], r.pass) || is_clamped(tiles[i + 1], r.pass))
                continue;
            auto const v0 = r.measured[i];
            auto const v1 = r.measured[i + 1];
            if (v0 <= value && v1 > value)
            {
                auto const l0 = unclamped_lod(tiles[i], r.pass);
                auto const l1 = unclamped_lod(tiles[i + 1], r.pass);
                return l0 + (value - v0) / (v1 - v0) * (l1 - l0);
            }
        }
        return -1;
    };

    for (auto row = 0; row < tile_rows; ++row)
    {
        if (ratio != 0 && tiles[row * tile_columns].ratio != ratio)
            continue;
        for (auto m = 0; m + 1 < pyramid_mips; ++m)
        {
            auto const half = level_at(row, float(m) + 0.5f);
            if (half < 0)
                continue;
            offset_sum += float(m) + 0.5f - half;
            ++out.crossings;
            auto const low = level_at(row, float(m) + 0.1f);
            auto const high = level_at(row, float(m) + 0.9f);
            if (low >= 0 && high >= 0)
            {
                width_sum += high - low;
                ++widths;
            }
        }
    }
    if (out.crossings > 0)
        out.offset = float(offset_sum / out.crossings);
    if (widths > 0)
        out.blend_width = float(width_sum / widths);
    return out;
}

cc::vector<pass> probe_passes()
{
    auto out = cc::vector<pass>();
    for (auto const n : anisotropies)
    {
        for (auto const bias : {0.0f, -0.5f, -1.0f, -2.0f})
            out.push_back({.anisotropy = n, .sampler_bias = bias, .path = path::implicit});
        for (auto const bias : {-0.5f, -1.0f, -2.0f})
            out.push_back({.anisotropy = n, .path = path::shader_bias, .shader_bias = bias});
        for (auto const bias : {0.0f, -1.0f})
            out.push_back({.anisotropy = n, .sampler_bias = bias, .path = path::gradients});
        for (auto const bias : {0.0f, -1.0f})
            out.push_back({.anisotropy = n, .sampler_bias = bias, .path = path::level});
    }
    return out;
}

sg::texture_2d create_pyramid(sg::context& ctx)
{
    auto const pyramid = ctx.persistent.create_texture_2d({
        .format = sg::pixel_format::r16_float,
        .width = pyramid_size,
        .height = pyramid_size,
        .mip_levels = pyramid_mips,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
    });
    auto cmd = ctx.create_command_list();
    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto const size = pyramid_size >> mip;
        auto const texels = cc::vector<u16>::create_filled(isize(size) * size, tg::f16(float(mip)).bits());
        cmd->upload.bytes_to_texture(pyramid.raw(), cc::as_bytes(cc::span<u16 const>(texels)),
                                     {.mip_level = mip, .array_layer = 0, .aspect = sg::texture_aspect::color});
    }
    ctx.submit_command_list(cc::move(cmd));
    return pyramid;
}

sg::sampler probe_sampler(int anisotropy, float bias)
{
    return {
        .min_filter = sg::sampler_filter::linear,
        .mag_filter = sg::sampler_filter::linear,
        .mip_filter = sg::sampler_filter::linear,
        .address_u = sg::sampler_address_mode::repeat,
        .address_v = sg::sampler_address_mode::repeat,
        .address_w = sg::sampler_address_mode::repeat,
        .mip_lod_bias = bias,
        .max_anisotropy = u32(anisotropy),
    };
}

cc::shared_async<cc::result<context_result>> measure(sg::context_handle ctx)
{
    auto const target = tg::vec2i(tile_columns * tile_pixels, tile_rows * tile_pixels);
    auto const tiles = probe_tiles();
    auto const passes = probe_passes();

    auto const building
        = ctx->cached.acquire_raster_pipeline(shaders::lod_probe::probe, {.lod = sg::pixel_format::r32_float});
    co_await cc::async_settled(building);
    auto const* const built = building->try_value();
    if (built == nullptr)
        co_return cc::error(pipeline_error(building));
    auto const pipeline = *built;
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::lod_probe::source>();

    auto const pyramid = create_pyramid(*ctx);
    auto const vertices
        = ctx->persistent.create_buffer_from_data(build_tile_vertices(tiles, target), sg::buffer_usage::vertex_buffer);
    auto const indices
        = ctx->persistent.create_buffer_from_data(build_tile_indices(int(tiles.size())), sg::buffer_usage::index_buffer);

    // One list per pass, each with its own target, so every readback is in flight before the first is awaited.
    // They all land in one epoch, so the readback ring is sized for all of them; the new size applies from the next epoch.
    ctx->download.set_budget(isize(passes.size()) * target[0] * target[1] * isize(sizeof(float)) + (isize(1) << 20));
    ctx->advance_epoch();
    auto readbacks = cc::vector<sg::bytes_future>();
    auto targets = cc::vector<sg::texture_2d>();
    for (auto const& p : passes)
    {
        auto const rt = ctx->persistent.create_texture_2d({
            .format = sg::pixel_format::r32_float,
            .width = target[0],
            .height = target[1],
            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
        });
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::lod_probe::source{.pyramid = pyramid.as_texture_view(),
                                       .smp = probe_sampler(p.anisotropy, p.sampler_bias)});
        {
            auto scope = cmd->raster.render_to(
                shaders::lod_probe::lod_target{.lod = rt.as_render_target_view().cleared(tg::vec4f(-1, 0, 0, 0))});
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.bind_vertex_buffers({vertices.as_vertex_buffer()});
            scope.bind_index_buffer(indices.as_index_buffer());
            scope.set_inline_constants(shaders::lod_probe::probe_constants{
                .mode = int(p.path),
                .shader_bias = p.shader_bias,
            }
                                           .to_block());
            scope.draw_indexed({.index_range = {.offset = 0, .size = int(tiles.size()) * 6}});
        }
        readbacks.push_back(cmd->download.bytes_from_texture(rt.raw()));
        ctx->submit_command_list(cc::move(cmd));
        targets.push_back(rt);
    }
    ctx->advance_epoch();

    auto result = context_result{
        .backend = cc::string(ctx->backend() == sg::backend_kind::dx12 ? "dx12" : "vulkan"),
        .adapter = ctx->metrics.adapter().name,
        .driver = ctx->metrics.adapter().driver_version,
    };
    for (auto i = 0; i < int(passes.size()); ++i)
    {
        auto const landing = readbacks[i].bytes();
        auto const& bytes = co_await landing;
        auto const floats = cc::span<float const>(reinterpret_cast<float const*>(bytes.span().data()),
                                                  bytes.span().size() / isize(sizeof(float)));
        result.passes.push_back({.pass = passes[i], .measured = tile_means(floats, target, int(tiles.size()))});
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion();
    co_return result;
}
} // namespace lod_probe
