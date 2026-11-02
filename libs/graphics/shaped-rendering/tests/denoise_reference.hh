#pragma once

#include "texel_formats.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>

// What the reference tests of the native denoise members share: the inputs they upload, and the readback they compare.
// The inputs are deterministic noise, so a failure reproduces exactly.

namespace sr_test::reference
{
using namespace cc::primitive_defines;

inline constexpr int k_w = 23;
inline constexpr int k_h = 17;

using vec3 = tg::vec<3, f64>;

inline constexpr auto k_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// The B3-spline taps every à-trous pass weights its 5x5 with.
inline constexpr f64 k_kernel[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

/// A deterministic value in [0, 1) per index and salt.
[[nodiscard]] inline f32 noise(int i, int salt)
{
    auto h = u32(i) * 2654435761u ^ u32(salt) * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return f32(h & 0xffffffu) / 16777216.0f;
}

/// A field of texels, every channel in [lo, hi).
[[nodiscard]] inline cc::vector<tg::vec4f> field(int salt, f32 lo, f32 hi)
{
    auto out = cc::vector<tg::vec4f>();
    for (auto i = 0; i < k_w * k_h; ++i)
        out.push_back(tg::vec4f(lo + (hi - lo) * noise(i * 4, salt), lo + (hi - lo) * noise(i * 4 + 1, salt),
                                lo + (hi - lo) * noise(i * 4 + 2, salt), lo + (hi - lo) * noise(i * 4 + 3, salt)));
    return out;
}

/// Normals of one surface, each tilted from +z by at most `spread` in x and y, with zero in w.
/// What an edge-stop at a high normal power sees on a real surface: neighbors that mostly pass.
/// Random directions would zero every tap and leave nothing for the depth and luminance stops to decide.
[[nodiscard]] inline cc::vector<tg::vec4f> clustered_normals(int salt, f32 spread = 0.05f)
{
    auto out = cc::vector<tg::vec4f>();
    for (auto i = 0; i < k_w * k_h; ++i)
        out.push_back(tg::vec4f(spread * (2.0f * noise(i * 2, salt) - 1.0f),
                                spread * (2.0f * noise(i * 2 + 1, salt) - 1.0f), 1.0f, 0.0f));
    return out;
}

[[nodiscard]] inline vec3 xyz(tg::vec4f v)
{
    return vec3(v[0], v[1], v[2]);
}

/// `a` and `b` multiplied channel by channel.
[[nodiscard]] inline vec3 times(vec3 a, vec3 b)
{
    return vec3(a[0] * b[0], a[1] * b[1], a[2] * b[2]);
}

[[nodiscard]] inline f64 luminance(vec3 c)
{
    return tg::dot(c, vec3(0.2126, 0.7152, 0.0722));
}

[[nodiscard]] inline bool inside(int x, int y)
{
    return x >= 0 && y >= 0 && x < k_w && y < k_h;
}

struct gpu_inputs;
} // namespace sr_test::reference

/// What a test hands the GPU: the textures, created and uploaded on `cmd`.
struct sr_test::reference::gpu_inputs
{
    sg::context& ctx;
    sg::command_list& cmd;

    [[nodiscard]] sg::texture_2d texture(cc::span<tg::vec4f const> texels,
                                         sg::pixel_format format = sg::pixel_format::rgba32_float)
    {
        auto t = ctx.persistent.create_texture_2d({.format = format, .width = k_w, .height = k_h, .usage = k_usage});
        auto const bytes = packed(format, texels);
        cmd.upload.bytes_to_texture(t.raw(), bytes);
        return t;
    }

    [[nodiscard]] sg::texture_2d target(sg::pixel_format format = sg::pixel_format::rgba32_float)
    {
        return ctx.persistent.create_texture_2d({.format = format, .width = k_w, .height = k_h, .usage = k_usage});
    }
};

namespace sr_test::reference
{
/// Reads `t` back as float4 texels, widening a narrower format with zeros.
[[nodiscard]] inline cc::shared_async<cc::vector<tg::vec4f>> read_back(sg::context& ctx, sg::texture_2d const& t)
{
    auto cmd = ctx.create_command_list();
    auto const bytes = cmd->download.bytes_from_texture(t.raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    auto const& data = co_await bytes.bytes();
    co_return unpacked(t.format(), data, isize(k_w) * k_h);
}

[[nodiscard]] inline cc::shared_async<sg::compute_pipeline_handle> settled(sg::async_compute_pipeline pipeline)
{
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    REQUIRE(built != nullptr).context("the pass did not build");
    co_return *built;
}
} // namespace sr_test::reference
