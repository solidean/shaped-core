#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>

// BCn block decoding (image/) — one 4x4 block of a block-compressed texture to plain texels.
//
// No container is involved: these take the 8 or 16 bytes of one block, wherever they came from.
// babel::dds is the reader that hands such blocks out; KTX2 and a GPU readback carry the same bytes.
//
// Every decoder is bit-exact against one stated rule rather than against "whatever a GPU does".
// Hardware disagrees in the last bit on BC1-BC5 interpolation, so the rule is the one the D3D specification and
// DirectXTex compute: interpolate the expanded endpoints exactly, then round to nearest.
// BC6H and BC7 specify their interpolation in integers, and there the rule is the specification's own.
//
// Texels come back row-major, texel (x, y) at index y * 4 + x, four values each in r, g, b, a order.
// A block at an image edge decodes all 16 texels, and cropping to the image is the caller's job.
//
// TWO-CHANNEL FORMATS LEAVE THE REST TO THE CONSUMER.
// BC4 decodes to (r, 0, 0, 1) and BC5 to (r, g, 0, 1), exactly what a GPU samples from them.
// A BC5 normal map therefore carries x and y only, and z = sqrt(1 - x^2 - y^2) is the consumer's to reconstruct.
//
//   auto const texels = babel::bcn::decode_bc3(babel::bcn::large_block(block_bytes));
//   auto const alpha_of_texel_5 = texels.rgba[5 * 4 + 3];

/// One decoded block as 8-bit values, 16 texels of r, g, b, a.
struct babel::bcn::rgba8_block
{
    cc::fixed_array<u8, 64> rgba = {};
};

/// One decoded block as floats, 16 texels of r, g, b, a.
struct babel::bcn::rgba32f_block
{
    cc::fixed_array<float, 64> rgba = {};
};

namespace babel::bcn
{
/// The 8 bytes of a BC1 or BC4 block.
/// A fixed extent rather than a checked length, so no decoder can be handed a short block.
using small_block = cc::fixed_span<byte const, 8>;

/// The 16 bytes of a BC2, BC3, BC5, BC6H or BC7 block.
using large_block = cc::fixed_span<byte const, 16>;

/// BC1 (DXT1): two RGB565 endpoints and 2-bit indices.
/// When the first endpoint is not greater than the second, the block is in its three-color mode: index 2 is the
/// midpoint, and index 3 is transparent black, the format's one bit of alpha.
[[nodiscard]] rgba8_block decode_bc1(small_block block);

/// BC2 (DXT2 / DXT3): explicit 4-bit alpha, then a BC1 color block.
/// The color half is always read in four-color mode, whatever its endpoint order says — BC1's alpha mode is BC1's alone.
[[nodiscard]] rgba8_block decode_bc2(large_block block);

/// BC3 (DXT4 / DXT5): a BC4-style alpha block, then a BC1 color block in four-color mode.
[[nodiscard]] rgba8_block decode_bc3(large_block block);

/// BC4 (ATI1): one interpolated channel, returned as (r, 0, 0, 255).
/// A signed block's values span -127..127, and land in the 8-bit result biased: -1 reads as 0, 0 as 128 and 1 as 255,
/// the same encoding an unsigned normal map uses.
/// decode_bc4_channel has the signed values themselves.
[[nodiscard]] rgba8_block decode_bc4(small_block block, bool is_signed);

/// BC5 (ATI2): two BC4 channels, the first block red and the second green, returned as (r, g, 0, 255).
/// Signed blocks are biased exactly as in decode_bc4.
[[nodiscard]] rgba8_block decode_bc5(large_block block, bool is_signed);

/// BC7: eight modes over one to three subsets, with optional channel rotation.
/// A reserved mode (a first byte of zero) decodes to transparent black, as the specification requires.
[[nodiscard]] rgba8_block decode_bc7(large_block block);

/// BC6H: half-float RGB, fourteen modes, alpha always 1.
/// Each value is the specification's half-float result, widened to a float without further rounding.
/// A reserved mode decodes to zero.
[[nodiscard]] rgba32f_block decode_bc6h(large_block block, bool is_signed);

/// BC1 - BC5 as floats: the interpolants exactly, before the rounding the 8-bit decoders apply.
/// Unsigned values land in 0..1 and signed BC4 / BC5 in -1..1, unbiased.
[[nodiscard]] rgba32f_block decode_bc1_f32(small_block block);
[[nodiscard]] rgba32f_block decode_bc2_f32(large_block block);
[[nodiscard]] rgba32f_block decode_bc3_f32(large_block block);
[[nodiscard]] rgba32f_block decode_bc4_f32(small_block block, bool is_signed);
[[nodiscard]] rgba32f_block decode_bc5_f32(large_block block, bool is_signed);

/// The one channel of a BC4 block, or of either half of a BC5 one, as the format's own integers.
/// 0..255 for an unsigned block and -127..127 for a signed one, where -128 is read as -127.
[[nodiscard]] cc::fixed_array<i16, 16> decode_bc4_channel(small_block block, bool is_signed);
} // namespace babel::bcn
