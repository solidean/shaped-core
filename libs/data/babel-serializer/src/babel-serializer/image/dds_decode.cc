#include <babel-serializer/image/bcn.hh>
#include <babel-serializer/image/dds.hh>
#include <babel-serializer/image/impl/dds_flags.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/half_float.hh>

// Decoding one subresource of a read DDS to plain texels; reading the container is dds.cc.

namespace babel::dds
{
namespace
{
using namespace babel::dds::impl;

/// One masked channel scaled to 0..1, or nothing when the mask is empty.
float masked_channel(u32 texel, u32 mask)
{
    auto const shift = cc::count_trailing_zeroes(mask);
    auto const width = cc::popcount(mask);
    auto const max_value = width >= 32 ? 4294967295.0 : double((u64(1) << width) - 1);
    return float(double((texel & mask) >> shift) / max_value);
}

/// A masked texel as r, g, b, a in 0..1.
void decode_masked_texel(pixel_format_header const& pf, u32 texel, float* out)
{
    auto const a_mask = effective_alpha_mask(pf);
    auto const alpha = a_mask != 0 ? masked_channel(texel, a_mask) : 1.0f;

    if ((pf.flags & pf_luminance) != 0)
    {
        auto const l = pf.r_mask != 0 ? masked_channel(texel, pf.r_mask) : 0.0f;
        out[0] = l;
        out[1] = l;
        out[2] = l;
    }
    else if ((pf.flags & pf_rgb) != 0)
    {
        out[0] = pf.r_mask != 0 ? masked_channel(texel, pf.r_mask) : 0.0f;
        out[1] = pf.g_mask != 0 ? masked_channel(texel, pf.g_mask) : 0.0f;
        out[2] = pf.b_mask != 0 ? masked_channel(texel, pf.b_mask) : 0.0f;
    }
    else
    {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
    }
    out[3] = alpha;
}

u8 to_unorm8(float v)
{
    auto const clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return u8(clamped * 255.0f + 0.5f);
}

/// Checks `sub` against what `tex` says it should hold, so decoding a hand-built one never reads out of bounds.
cc::result<cc::unit> check_subresource(data const& tex, subresource const& sub)
{
    if (sub.width <= 0 || sub.height <= 0 || sub.depth <= 0)
        return cc::error(cc::format("dds decode: subresource has a non-positive extent {}x{}x{}", //
                                    sub.width, sub.height, sub.depth));
    auto const needed = tex.slice_byte_size(sub.width, sub.height) * isize(sub.depth);
    if (needed <= 0)
        return cc::error(cc::format("dds decode: {} has no byte size", to_string(tex.format)));
    if (sub.bytes.size() < needed)
        return cc::error(cc::format("dds decode: subresource holds {} of the {} bytes its extent needs", //
                                    sub.bytes.size(), needed));
    return cc::unit{};
}

/// Calls `fn(block_bytes, block_x, block_y, slice)` for every block of a block-compressed subresource.
template <class Fn>
void for_each_block(data const& tex, subresource const& sub, Fn&& fn)
{
    auto const block_bytes = tex.row_pitch(4); // one block
    auto const blocks_x = (sub.width + 3) / 4;
    auto const blocks_y = (sub.height + 3) / 4;
    auto const bytes = sub.bytes.span();
    auto offset = isize(0);
    for (auto z = 0; z < sub.depth; ++z)
        for (auto by = 0; by < blocks_y; ++by)
            for (auto bx = 0; bx < blocks_x; ++bx)
            {
                fn(bytes.subspan({.offset = offset, .size = block_bytes}), bx, by, z);
                offset += block_bytes;
            }
}

/// Copies the texels of a decoded block that lie inside the subresource into `out`.
template <class T>
void store_block(cc::span<T const> block_texels, subresource const& sub, int bx, int by, int z, T* out)
{
    for (auto y = 0; y < 4; ++y)
    {
        auto const py = by * 4 + y;
        if (py >= sub.height)
            break;
        for (auto x = 0; x < 4; ++x)
        {
            auto const px = bx * 4 + x;
            if (px >= sub.width)
                break;
            auto const dst = ((isize(z) * sub.height + py) * sub.width + px) * 4;
            for (auto c = 0; c < 4; ++c)
                out[dst + c] = block_texels[(y * 4 + x) * 4 + c];
        }
    }
}
} // namespace

// decoding
// -------------------------------------------------------------------------------------------------

cc::result<cc::vector<byte>> decode_rgba8(data const& tex, subresource const& sub)
{
    CC_RECORD_SCOPE("dds.decode_rgba8");

    if (is_float(tex.format))
        return cc::error(cc::format("dds decode: {} holds floats; decode it with decode_rgba32f", to_string(tex.format)));
    CC_RETURN_IF_ERROR(check_subresource(tex, sub));

    auto const texel_count = isize(sub.width) * sub.height * sub.depth;
    auto out = cc::vector<byte>::create_uninitialized(texel_count * 4);
    auto* const dst = reinterpret_cast<u8*>(out.data());

    if (is_block_compressed(tex.format))
    {
        auto const fmt = tex.format;
        for_each_block(tex, sub,
                       [&](cc::span<byte const> block, int bx, int by, int z)
                       {
                           auto texels = bcn::rgba8_block{};
                           switch (fmt)
                           {
                           case pixel_format::bc1_unorm:
                           case pixel_format::bc1_unorm_srgb:
                               texels = bcn::decode_bc1(block);
                               break;
                           case pixel_format::bc2_unorm:
                           case pixel_format::bc2_unorm_srgb:
                               texels = bcn::decode_bc2(block);
                               break;
                           case pixel_format::bc3_unorm:
                           case pixel_format::bc3_unorm_srgb:
                               texels = bcn::decode_bc3(block);
                               break;
                           case pixel_format::bc4_unorm:
                           case pixel_format::bc4_snorm:
                               texels = bcn::decode_bc4(block, fmt == pixel_format::bc4_snorm);
                               break;
                           case pixel_format::bc5_unorm:
                           case pixel_format::bc5_snorm:
                               texels = bcn::decode_bc5(block, fmt == pixel_format::bc5_snorm);
                               break;
                           default:
                               texels = bcn::decode_bc7(block);
                               break;
                           }
                           store_block<u8>(cc::span<u8 const>(texels.rgba), sub, bx, by, z, dst);
                       });
        return cc::move(out);
    }

    auto const src = sub.bytes.span();
    auto const bytes_per_texel = tex.row_pitch(1);
    auto const row_pitch = tex.row_pitch(sub.width);
    auto const slice_size = tex.slice_byte_size(sub.width, sub.height);

    for (auto z = 0; z < sub.depth; ++z)
        for (auto y = 0; y < sub.height; ++y)
        {
            auto const row = isize(z) * slice_size + isize(y) * row_pitch;
            for (auto x = 0; x < sub.width; ++x)
            {
                auto const at = row + isize(x) * bytes_per_texel;
                auto* const o = dst + ((isize(z) * sub.height + y) * sub.width + x) * 4;
                switch (tex.format)
                {
                case pixel_format::rgba8_unorm:
                case pixel_format::rgba8_unorm_srgb:
                    o[0] = u8(src[at + 0]);
                    o[1] = u8(src[at + 1]);
                    o[2] = u8(src[at + 2]);
                    o[3] = u8(src[at + 3]);
                    break;
                case pixel_format::bgra8_unorm:
                case pixel_format::bgra8_unorm_srgb:
                    o[0] = u8(src[at + 2]);
                    o[1] = u8(src[at + 1]);
                    o[2] = u8(src[at + 0]);
                    o[3] = u8(src[at + 3]);
                    break;
                case pixel_format::bgrx8_unorm:
                case pixel_format::bgrx8_unorm_srgb:
                    o[0] = u8(src[at + 2]);
                    o[1] = u8(src[at + 1]);
                    o[2] = u8(src[at + 0]);
                    o[3] = 255;
                    break;
                case pixel_format::r8_unorm:
                    o[0] = u8(src[at]);
                    o[1] = 0;
                    o[2] = 0;
                    o[3] = 255;
                    break;
                case pixel_format::rg8_unorm:
                    o[0] = u8(src[at + 0]);
                    o[1] = u8(src[at + 1]);
                    o[2] = 0;
                    o[3] = 255;
                    break;
                default:
                {
                    // Masked: up to four bytes, little-endian, read byte by byte so a 24-bit texel stays in bounds.
                    auto texel = u32(0);
                    for (auto b = isize(0); b < bytes_per_texel; ++b)
                        texel |= u32(u8(src[at + b])) << (8 * b);
                    float rgba[4] = {};
                    decode_masked_texel(tex.file_header.pixels, texel, rgba);
                    for (auto c = 0; c < 4; ++c)
                        o[c] = to_unorm8(rgba[c]);
                    break;
                }
                }
            }
        }

    return cc::move(out);
}

cc::result<cc::vector<float>> decode_rgba32f(data const& tex, subresource const& sub)
{
    CC_RECORD_SCOPE("dds.decode_rgba32f");
    CC_RETURN_IF_ERROR(check_subresource(tex, sub));

    auto const texel_count = isize(sub.width) * sub.height * sub.depth;
    auto const fmt = tex.format;

    if (fmt == pixel_format::bc6h_ufloat || fmt == pixel_format::bc6h_sfloat)
    {
        auto out = cc::vector<float>::create_uninitialized(texel_count * 4);
        for_each_block(tex, sub,
                       [&](cc::span<byte const> block, int bx, int by, int z)
                       {
                           auto const texels = bcn::decode_bc6h(block, fmt == pixel_format::bc6h_sfloat);
                           store_block<float>(cc::span<float const>(texels.rgba), sub, bx, by, z, out.data());
                       });
        return cc::move(out);
    }

    if (fmt == pixel_format::bc4_snorm || fmt == pixel_format::bc5_snorm)
    {
        // The signed values themselves, which the 8-bit decode can only carry biased.
        auto out = cc::vector<float>::create_uninitialized(texel_count * 4);
        for_each_block(tex, sub,
                       [&](cc::span<byte const> block, int bx, int by, int z)
                       {
                           auto texels = bcn::rgba32f_block{};
                           auto const red = bcn::decode_bc4_channel(block, true);
                           auto green = cc::fixed_array<i16, 16>{};
                           if (fmt == pixel_format::bc5_snorm)
                               green = bcn::decode_bc4_channel(block.subspan(8), true);
                           for (auto i = 0; i < 16; ++i)
                           {
                               texels.rgba[i * 4 + 0] = float(red[i]) / 127.0f;
                               texels.rgba[i * 4 + 1] = float(green[i]) / 127.0f;
                               texels.rgba[i * 4 + 3] = 1.0f;
                           }
                           store_block<float>(cc::span<float const>(texels.rgba), sub, bx, by, z, out.data());
                       });
        return cc::move(out);
    }

    if (fmt == pixel_format::masked)
    {
        auto out = cc::vector<float>::create_uninitialized(texel_count * 4);
        auto const src = sub.bytes.span();
        auto const bytes_per_texel = isize(tex.file_header.pixels.rgb_bit_count / 8);
        auto const row_pitch = tex.row_pitch(sub.width);
        auto const slice_size = tex.slice_byte_size(sub.width, sub.height);
        for (auto z = 0; z < sub.depth; ++z)
            for (auto y = 0; y < sub.height; ++y)
                for (auto x = 0; x < sub.width; ++x)
                {
                    auto const at = isize(z) * slice_size + isize(y) * row_pitch + isize(x) * bytes_per_texel;
                    auto texel = u32(0);
                    for (auto b = isize(0); b < bytes_per_texel; ++b)
                        texel |= u32(u8(src[at + b])) << (8 * b);
                    decode_masked_texel(tex.file_header.pixels, texel,
                                        out.data() + ((isize(z) * sub.height + y) * sub.width + x) * 4);
                }
        return cc::move(out);
    }

    if (is_float(fmt))
    {
        // r16 / rgba16 / r32 / rgba32 floats: a missing channel is 0, and a missing alpha 1.
        auto out = cc::vector<float>::create_uninitialized(texel_count * 4);
        auto const src = sub.bytes.span();
        auto const channels = fmt == pixel_format::r16_float || fmt == pixel_format::r32_float ? 1 : 4;
        auto const is_half = fmt == pixel_format::r16_float || fmt == pixel_format::rgba16_float;
        auto const sample_bytes = isize(is_half ? 2 : 4);
        for (auto t = isize(0); t < texel_count; ++t)
        {
            float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (auto c = 0; c < channels; ++c)
            {
                auto const at = (t * channels + c) * sample_bytes;
                rgba[c] = is_half ? float(tg::f16::make_from_bits(cc::load_bytes_le<u16>(src, at)))
                                  : cc::load_bytes_le<float>(src, at);
            }
            for (auto c = 0; c < 4; ++c)
                out[t * 4 + c] = rgba[c];
        }
        return cc::move(out);
    }

    // Everything else is 8 bits a channel, so the 8-bit decode loses nothing.
    auto bytes = decode_rgba8(tex, sub);
    CC_RETURN_IF_ERROR(bytes);
    auto out = cc::vector<float>::create_uninitialized(texel_count * 4);
    for (auto i = isize(0); i < texel_count * 4; ++i)
        out[i] = float(u8(bytes.value()[i])) / 255.0f;
    return cc::move(out);
}
} // namespace babel::dds
