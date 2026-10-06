#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

/// Block-compresses a texture on the GPU, one mip level per call: BC1, BC3, BC4, BC5 or BC7, as the target's format says.
///
/// A block-compressed texture is sampled at no cost beyond its smaller footprint, so a texture that is only ever
/// sampled gives up little and keeps a quarter or an eighth of its memory:
///
///   - **BC1** — RGB at 4 bits per texel, with an optional one-bit alpha; the cheapest color, and the coarsest.
///   - **BC3** — RGBA at 8: BC1's color beside a separate smooth alpha.
///   - **BC4** — one channel at 4 bits, from the source's red: roughness, occlusion, height.
///   - **BC5** — two channels at 8, from red and green: a tangent-space normal's x and y.
///   - **BC7** — RGBA at 8, and the best quality of them: around 40 dB on photographic content.
///
/// The encoders are `shaders/compression/bc.sgl`.
///
/// **A level passes through a transient buffer**, because no API lets a shader write a block-compressed texture.
/// The blocks are copied in with `cmd.copy.buffer_to_texture`, so that buffer, 8 or 16 bytes per block, counts against
/// the context's transient budget until the epoch ends — 16 MiB for the top level of a 4096x4096 BC7 texture.
///
/// The device must have `sg::feature::texture_compression_bc`; without it the target cannot even be created.
///
/// A render routine **parametrized on the target's format family**, its `_unorm` member: one compute pipeline each.
class sr::bc_compress_routine : public sg::render_routine<bc_compress_routine, sg::pixel_format>
{
public:
    /// Encodes level `level` of `source` and writes it into the same level of `target`.
    ///
    /// `source` has `texture` usage and is read as float4: the encoders take red for BC4, red and green for BC5.
    /// `target` has a format `is_supported_target` accepts, `copy_dst` usage, and the same extent.
    /// The target is sRGB exactly when the source is; an sRGB source is encoded as the bytes it stores, so the two
    /// decode to the same colors.
    /// Declines while the encoder is still compiling, and after a compile that failed.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd,
                                                     sg::texture_2d const& source,
                                                     sg::texture_2d const& target,
                                                     int level);

    /// Whether `format` is one this routine writes: BC1, BC3, BC4, BC5 and BC7 in their unsigned forms, sRGB included.
    [[nodiscard]] static bool is_supported_target(sg::pixel_format format);

    /// `family`, a supported target, in the sRGB variant where `source` is sRGB and the family has one.
    /// BC4 and BC5 have none, so they are only ever the target of a linear source.
    [[nodiscard]] static sg::pixel_format target_for(sg::pixel_format source, sg::pixel_format family);

    /// Whether `execute` can write `target` from `source` here: a device with BC textures, a supported target, and an
    /// sRGB-ness the two agree on.
    [[nodiscard]] static bool can_compress(sg::context const& ctx, sg::pixel_format source, sg::pixel_format target);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
