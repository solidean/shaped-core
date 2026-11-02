#pragma once

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/jitter_sequence.hh>
#include <shaped-rendering/reconstruct.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

// The scene the upscaling members' image tests trace, and the metric they are judged by.
//
// A stripe pattern near the input's resolution limit, crossed by one hard edge, traced at 32 x 32 and measured at the
// 64 x 64 output's pixel centres.

namespace sr_test::upscale_scene
{
using namespace cc::primitive_defines;

struct wall_guides;     // the guides of a flat wall facing the camera
struct denoised_result; // what run_denoised hands back

inline constexpr int k_in = 32;
inline constexpr int k_out = 64;

inline constexpr auto image_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// Stripes near the input's resolution limit, crossed by one hard diagonal edge, in [0, 1] on both axes.
[[nodiscard]] inline f32 scene(f32 u, f32 v)
{
    auto const stripes = 0.5f + 0.4f * tg::sin(tg::angle_f::make_from_degree(u * 360.0f * 11.0f));
    return u + v < 1.0f ? stripes : 0.15f + 0.6f * stripes;
}

/// One traced frame: every pixel sampled once at its centre plus `jitter`, the scene shifted right by `shift` input pixels.
[[nodiscard]] inline cc::vector<tg::vec4f> trace(tg::vec2f jitter, f32 shift)
{
    auto pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_in; ++y)
        for (auto x = 0; x < k_in; ++x)
        {
            auto const u = (f32(x) + 0.5f + jitter[0] - shift) / f32(k_in);
            auto const v = (f32(y) + 0.5f + jitter[1]) / f32(k_in);
            auto const s = scene(u, v);
            pixels.push_back(tg::vec4f(s, s, s, 1));
        }
    return pixels;
}

/// The error against the scene at the output's pixel centres, shifted as the last traced frame was.
[[nodiscard]] inline f32 rmse_against_scene(cc::span<tg::vec4f const> pixels, f32 shift)
{
    // A border of four output pixels is left out: an upscaler has nothing to reconstruct an edge pixel from but clamps.
    auto sum = 0.0f;
    auto count = 0;
    for (auto y = 4; y < k_out - 4; ++y)
        for (auto x = 4; x < k_out - 4; ++x)
        {
            auto const u = (f32(x) + 0.5f) / f32(k_out) - shift / f32(k_in);
            auto const v = (f32(y) + 0.5f) / f32(k_out);
            auto const d = pixels[y * k_out + x][0] - scene(u, v);
            sum += d * d;
            ++count;
        }
    return tg::sqrt(sum / f32(count));
}

inline void upload(sg::command_list& cmd, sg::texture_2d const& tex, cc::span<tg::vec4f const> pixels)
{
    cmd.upload.bytes_to_texture(tex.raw(), pixels.as_bytes());
}

[[nodiscard]] inline cc::vector<tg::vec4f> filled(int size, tg::vec4f value)
{
    return cc::vector<tg::vec4f>::create_filled(size * size, value);
}

[[nodiscard]] inline sg::texture_2d make_image(sg::context& ctx, int size, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d({.format = format, .width = size, .height = size, .usage = image_usage});
}

/// A hash of the pixel and the frame in [-1, 1), so the noise is uncorrelated between pixels and between frames.
[[nodiscard]] inline f32 noise(int x, int y, int frame)
{
    auto h = u32(x) * 73856093u ^ u32(y) * 19349663u ^ u32(frame) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return f32(h & 0xffffu) / 32768.0f - 1.0f;
}

/// `trace` as an albedo lit by an irradiance of 1 that every sample gets wrong by up to `amplitude`, uniformly.
/// The scene's detail is the albedo's, as a renderer's texture is, so only the lighting is noisy.
[[nodiscard]] inline cc::vector<tg::vec4f> trace_noisy(tg::vec2f jitter, int frame, f32 amplitude)
{
    auto pixels = trace(jitter, 0.0f);
    for (auto y = 0; y < k_in; ++y)
        for (auto x = 0; x < k_in; ++x)
        {
            auto const s = pixels[y * k_in + x][0] * (1.0f + amplitude * noise(x, y, frame));
            pixels[y * k_in + x] = tg::vec4f(s, s, s, 1);
        }
    return pixels;
}

/// A camera looking down +z with a 60 degree vertical field of view, as a left-handed caller has it.
[[nodiscard]] inline tg::mat4f test_projection()
{
    auto p = tg::mat4f::zero;
    p[0, 0] = 1.7320508f;
    p[1, 1] = 1.7320508f;
    p[2, 2] = 1.0f;
    p[2, 3] = 1.0f;
    p[3, 2] = -0.1f;
    return p;
}

} // namespace sr_test::upscale_scene

/// The guides of a flat wall facing the camera, one unit away, with the scene as its albedo.
struct sr_test::upscale_scene::wall_guides
{
    sg::texture_2d albedo;
    sg::texture_2d specular_albedo;
    sg::texture_2d normal;
    sg::texture_2d roughness;
    sg::texture_2d depth;
    sg::texture_2d motion;

    [[nodiscard]] static wall_guides make(sg::context& ctx, int size)
    {
        return {.albedo = make_image(ctx, size, sg::pixel_format::rgba32_float),
                .specular_albedo = make_image(ctx, size, sg::pixel_format::rgba32_float),
                .normal = make_image(ctx, size, sg::pixel_format::rgba32_float),
                .roughness = make_image(ctx, size, sg::pixel_format::rgba32_float),
                .depth = make_image(ctx, size, sg::pixel_format::rgba32_float),
                .motion = make_image(ctx, size, sg::pixel_format::rgba32_float)};
    }

    void upload_to(sg::command_list& cmd, int size, tg::vec2f jitter) const
    {
        upload(cmd, albedo, trace(jitter, 0.0f));
        upload(cmd, specular_albedo, filled(size, tg::vec4f(0.04f, 0.04f, 0.04f, 0)));
        upload(cmd, normal, filled(size, tg::vec4f(0, 0, -1, 0)));
        upload(cmd, roughness, filled(size, tg::vec4f(1, 0, 0, 0)));
        upload(cmd, depth, filled(size, tg::vec4f(1, 0, 0, 0)));
        upload(cmd, motion, filled(size, tg::vec4f(0, 0, 0, 0)));
    }

    [[nodiscard]] sr::reconstruct_guides guides(tg::vec2f jitter) const
    {
        return {.albedo = albedo,
                .specular_albedo = specular_albedo,
                .normal = normal,
                .roughness = roughness,
                .depth = depth,
                .motion = motion,
                .jitter = jitter,
                .view_to_clip = test_projection()};
    }
};

struct sr_test::upscale_scene::denoised_result
{
    sr::reconstruct_outcome first;
    sr::reconstruct_outcome last;
    cc::vector<tg::vec4f> output;
};

namespace sr_test::upscale_scene
{

/// Traces `frames` noisy frames of the still scene and denoises them through the front, naming `method`.
/// The jitter is the Halton sequence `sr::reconstruct_jitter` hands a member that upscales by itself; `jitter_sign` of
/// -1 hands the member that jitter the wrong way round.
inline cc::shared_async<denoised_result> run_denoised(sg::context& ctx,
                                                      sr::denoise_method method,
                                                      int frames,
                                                      f32 amplitude,
                                                      int out_size,
                                                      f32 jitter_sign = 1.0f)
{
    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const wall = wall_guides::make(ctx, k_in);
    auto const output = make_image(ctx, out_size, sg::pixel_format::rgba32_float);
    auto const settings = sr::reconstruct_settings{.denoiser = method, .fresh_samples = true};

    auto history = sr::reconstruct_history();
    auto result = denoised_result{};
    auto readback = sg::data_future<tg::vec4f>();
    for (auto frame = 0; frame < frames; ++frame)
    {
        auto const jitter = out_size == k_in
                              ? tg::vec2f(0, 0)
                              : sr::impl::halton_jitter(u32(frame), tg::vec2i(k_in, k_in), tg::vec2i(out_size, out_size));
        auto cmd = ctx.create_command_list();
        upload(*cmd, color, trace_noisy(jitter, frame, amplitude));
        wall.upload_to(*cmd, k_in, jitter);
        auto const outcome = sr::reconstruct_routine::execute(
            *cmd, {.color = color, .guides = wall.guides(jitter * jitter_sign), .output = output}, history, settings);
        if (frame == 0)
            result.first = outcome;
        result.last = outcome;
        if (frame + 1 == frames)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
    }

    auto const pixels = co_await readback.data();
    for (auto i = isize(0); i < pixels.size(); ++i)
        result.output.push_back(pixels[i]);
    co_return result;
}
} // namespace sr_test::upscale_scene
