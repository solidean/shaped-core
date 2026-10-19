#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
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

/// Creates a stream's scaler; null when MetalFX refuses the descriptor or `output_format`'s usage falls short of what
/// the scaler needs, both of which are logged.
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
/// False when the output's usage falls short of the scaler's needs; nothing reached `desc.output` then.
[[nodiscard]] bool metalfx_dispatch(metalfx_stream& stream, sg::command_list& cmd, metalfx_dispatch_desc const& desc);
} // namespace sr::impl
