#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

#include <memory>

// The seam between sr's MetalFX routine and Apple's framework.
//
// Every Metal and MetalFX type stays inside impl/metalfx_backend.cc; this header names none, so the routine compiles
// the same everywhere, and impl/metalfx_null.cc answers for a build without the Metal backend.

namespace sr::impl
{
/// Whether this build carries the Metal backend and `ctx` is a Metal context whose device runs MetalFX's MTL4
/// temporal scaler.
[[nodiscard]] bool metalfx_available(sg::context const& ctx);

/// The largest output-to-input ratio, per axis, the scaler on `ctx`'s device accepts; 0 where it is not available.
[[nodiscard]] f32 metalfx_max_scale(sg::context const& ctx);

/// One stream's MetalFX: the temporal scaler created for these extents and formats.
class metalfx_stream;

/// What a stream is created for.
struct metalfx_stream_desc
{
    tg::vec2i input_extent;
    tg::vec2i output_extent;
    sg::pixel_format color_format = sg::pixel_format::undefined;
    sg::pixel_format output_format = sg::pixel_format::undefined;
};

/// Creates a stream's scaler; null, logged, when MetalFX refuses the descriptor.
/// Its release waits for the GPU, so a stream may be dropped mid-frame.
[[nodiscard]] std::shared_ptr<metalfx_stream> metalfx_create_stream(sg::context& ctx, metalfx_stream_desc const& desc);

/// One upscale's textures, every one at the stream's extents and formats.
/// `depth`, `motion` and `exposure` are metalfx_prepare.sgl's output.
struct metalfx_dispatch_desc
{
    sg::texture_2d color;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d exposure;
    sg::texture_2d output;

    /// sr's jitter: where each sample lies, in input pixels.
    tg::vec2f jitter;
    bool reset = false;
};

/// Encodes one upscale into `cmd` through sg's Metal native scope.
/// False when the output's usage falls short of the scaler's needs, logged once per stream; nothing reached
/// `desc.output` then.
[[nodiscard]] bool metalfx_dispatch(metalfx_stream& stream, sg::command_list& cmd, metalfx_dispatch_desc const& desc);

// MetalFX's temporal denoised scaler, which denoises and upscales in one pass.

/// Whether `ctx` is a Metal context whose device runs the MTL4 denoised scaler.
[[nodiscard]] bool metalfx_denoised_available(sg::context const& ctx);

/// The largest output-to-input ratio, per axis, the denoised scaler accepts; 0 where it is not available.
[[nodiscard]] f32 metalfx_denoised_max_scale(sg::context const& ctx);

/// One stream's denoised scaler.
class metalfx_denoised_stream;

/// What a denoised stream is created for: the extents and every texture's format.
struct metalfx_denoised_stream_desc
{
    tg::vec2i input_extent;
    tg::vec2i output_extent;
    sg::pixel_format color_format = sg::pixel_format::undefined;
    sg::pixel_format albedo_format = sg::pixel_format::undefined;
    sg::pixel_format specular_albedo_format = sg::pixel_format::undefined;
    sg::pixel_format normal_format = sg::pixel_format::undefined;
    sg::pixel_format roughness_format = sg::pixel_format::undefined;
    sg::pixel_format output_format = sg::pixel_format::undefined;
};

/// Creates a stream's denoised scaler; null, logged, when MetalFX refuses the descriptor.
/// Its release waits for the GPU, as `metalfx_create_stream`'s does.
[[nodiscard]] std::shared_ptr<metalfx_denoised_stream> metalfx_create_denoised_stream(
    sg::context& ctx,
    metalfx_denoised_stream_desc const& desc);

/// One denoise's textures and camera, at the stream's extents and formats.
struct metalfx_denoised_dispatch_desc
{
    sg::texture_2d color;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d exposure;
    sg::texture_2d albedo;
    sg::texture_2d specular_albedo;
    sg::texture_2d normal;
    sg::texture_2d roughness;
    sg::texture_2d output;

    /// sr's jitter, in input pixels.
    tg::vec2f jitter;

    /// The camera; `view_to_clip` is the one `metalfx_view_to_clip` makes, matching the depth texture.
    tg::mat4f world_to_view = tg::mat4f::identity;
    tg::mat4f view_to_clip = tg::mat4f::identity;

    bool reset = false;
};

/// Encodes one denoise into `cmd` through sg's Metal native scope.
/// False when a texture's usage falls short of the scaler's needs, logged once per stream; nothing reached
/// `desc.output` then.
[[nodiscard]] bool metalfx_denoised_dispatch(metalfx_denoised_stream& stream,
                                             sg::command_list& cmd,
                                             metalfx_denoised_dispatch_desc const& desc);
} // namespace sr::impl
