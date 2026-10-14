#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

/// What BC1 does with a texel's alpha.
enum class sr::bc1_alpha : cc::u8
{
    /// alpha is ignored and every texel is opaque — the cheap color format it usually is
    opaque,
    /// a texel under half alpha decodes as transparent black, through BC1's three-color mode
    cut_out,
};

/// The range a BC4 or BC5 source's channels are read in.
enum class sr::bc_source_range : cc::u8
{
    /// 0..1, clamped: roughness, occlusion, height
    zero_to_one,
    /// -1..1, clamped: a tangent-space normal's x and y, a signed offset.
    /// An unsigned target stores it as n * 0.5 + 0.5, which whoever samples it undoes; a signed target stores it as is.
    minus_one_to_one,
};

/// Encode knobs.
struct sr::bc_compress_options
{
    /// Opaque by default: a source whose alpha means nothing would otherwise come out with holes wherever it is low.
    sr::bc1_alpha bc1_alpha = sr::bc1_alpha::opaque;

    /// How BC4 reads its source's red.
    sr::bc_source_range bc4_range = sr::bc_source_range::zero_to_one;

    /// How BC5 reads its source's red and green.
    sr::bc_source_range bc5_range = sr::bc_source_range::zero_to_one;
};

/// Block-compresses a texture on the GPU, one mip level per call: BC1, BC3, BC4, BC5 or BC7, as the target's format says.
///
/// A block-compressed texture is sampled at no cost beyond its smaller footprint, so a texture that is only ever
/// sampled gives up little and keeps a quarter or an eighth of its memory:
///
///   - **BC1** — RGB at 4 bits per texel, with an optional one-bit alpha (`bc_compress_options::bc1_alpha`); the cheapest color, and the coarsest.
///   - **BC3** — RGBA at 8: BC1's color beside a separate smooth alpha.
///   - **BC4** — one channel at 4 bits, from the source's red: roughness, occlusion, height.
///   - **BC5** — two channels at 8, from red and green: a tangent-space normal's x and y.
///   - **BC7** — RGBA at 8, and the best quality of them: around 40 dB on the tests' noisy gradient.
///
/// BC4 and BC5 come in an unsigned and a signed form, and `bc_compress_options` says what range their source is in.
/// Block compression stores normalized or float data only, so an integer or a depth source has no target here.
///
/// The encoders are `shaders/compression/bc.sgl`.
///
/// **A level passes through a transient buffer**, because sg offers a shader no way to write a block-compressed texture.
/// The blocks are copied in with `cmd.copy.buffer_to_texture`, so that buffer, 8 or 16 bytes per block, counts against
/// the context's transient budget until the epoch ends — 16 MiB for the top level of a 4096x4096 BC7 texture.
///
/// The device must have `sg::feature::texture_compression_bc`; without it the target cannot even be created.
///
/// A render routine **parametrized on the target's format family**, its unsigned linear member: one compute pipeline each.
class sr::bc_compress_routine : public sg::render_routine<bc_compress_routine, sg::pixel_format>
{
public:
    /// Encodes level `level` of `source` and writes it into the same level of `target`.
    ///
    /// `source` has `texture` usage and is read as float4: the encoders take red for BC4, red and green for BC5.
    /// `target` has a format `is_supported_target` accepts, `copy_dst` usage, and the same extent as `source`.
    /// The target is sRGB exactly when the source is; an sRGB source is encoded as the bytes it stores, so the two
    /// decode to the same colors.
    /// Declines while the encoder is still compiling, and after a compile that failed.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd,
                                                     sg::texture_2d const& source,
                                                     sg::texture_2d const& target,
                                                     int level,
                                                     bc_compress_options const& opts = {});

    /// Whether `format` is one this routine writes: BC1, BC3 and BC7 unsigned with their sRGB forms, and BC4 and BC5
    /// in both their unsigned and their signed forms.
    [[nodiscard]] static bool is_supported_target(sg::pixel_format format);

    /// `family`, a supported target, in the sRGB variant where `source` is sRGB and the family has one.
    /// BC4 and BC5 have none, so they are only ever the target of a linear source.
    [[nodiscard]] static sg::pixel_format target_for(sg::pixel_format source, sg::pixel_format family);

    /// Whether `execute` can write `target` from `source` here: a device with BC textures, a supported target, a source
    /// that is neither block-compressed, integer nor depth, and an sRGB-ness the two agree on.
    [[nodiscard]] static bool can_compress(sg::context const& ctx, sg::pixel_format source, sg::pixel_format target);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
