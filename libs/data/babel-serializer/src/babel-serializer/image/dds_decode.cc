#include <babel-serializer/image/bcn.hh>
#include <babel-serializer/image/dds.hh>
#include <babel-serializer/image/impl/dds_flags.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/half_float.hh>

// Decoding one subresource of a read DDS to plain texels; reading the container is dds.cc.

namespace babel::dds
{
namespace
{
using namespace babel::dds::impl;

/// One masked channel scaled to 0..1.
/// The mask is one run of bits — the reader refuses any other — so the shifted value never exceeds its maximum.
float masked_channel(u32 texel, u32 mask)
{
    auto const shift = cc::count_trailing_zeroes(mask);
    auto const width = cc::popcount(mask);
    auto const max_value = width >= 32 ? 4294967295.0 : double((u64(1) << width) - 1);
    return float(double((texel & mask) >> shift) / max_value);
}

/// A masked texel as r, g, b, a in 0..1.
cc::fixed_array<float, 4> decode_masked_texel(pixel_format_header const& pf, u32 texel)
{
    auto const a_mask = effective_alpha_mask(pf);
    auto out = cc::fixed_array<float, 4>{0.0f, 0.0f, 0.0f, a_mask != 0 ? masked_channel(texel, a_mask) : 1.0f};

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
    return out;
}

/// The texel of a masked format at byte `at`: up to four bytes, little-endian, read byte by byte so a 24-bit texel
/// stays in bounds.
u32 load_masked_texel(cc::span<byte const> src, isize at, isize bytes_per_texel)
{
    auto texel = u32(0);
    for (auto b = isize(0); b < bytes_per_texel; ++b)
        texel |= u32(u8(src[at + b])) << (8 * b);
    return texel;
}

u8 to_unorm8(float v)
{
    auto const clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return u8(clamped * 255.0f + 0.5f);
}

/// Checks `sub` against what `tex` says it should hold, and the decoded size against the budget.
/// `data` and `subresource` are plain structs a caller may build by hand, so nothing here trusts that `read` made them.
cc::result<cc::unit> check_subresource(data const& tex, subresource const& sub, isize sample_bytes, decode_options opts)
{
    if (sub.width <= 0 || sub.height <= 0 || sub.depth <= 0)
        return cc::error(cc::format("dds decode: subresource has a non-positive extent {}x{}x{}", //
                                    sub.width, sub.height, sub.depth));
    if (u32(sub.width) > max_extent || u32(sub.height) > max_extent || u32(sub.depth) > max_extent)
        return cc::error(cc::format("dds decode: implausible extent {}x{}x{}", sub.width, sub.height, sub.depth));

    if (tex.format == pixel_format::masked)
    {
        auto const bits = tex.file_header.pixels.rgb_bit_count;
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
            return cc::error(cc::format("dds decode: a masked pixel format of {} bits per pixel", bits));
    }

    auto const decoded = isize(sub.width) * sub.height * sub.depth * 4 * sample_bytes;
    if (decoded > opts.max_decoded_bytes)
        return cc::error(cc::format("dds decode: {}x{}x{} decodes to {} bytes, past the {}-byte budget", //
                                    sub.width, sub.height, sub.depth, decoded, opts.max_decoded_bytes));

    auto const needed = tex.slice_byte_size(sub.width, sub.height) * isize(sub.depth);
    if (needed <= 0)
        return cc::error(cc::format("dds decode: {} has no byte size", to_string(tex.format)));
    if (sub.bytes.size() < needed)
        return cc::error(cc::format("dds decode: subresource holds {} of the {} bytes its extent needs", //
                                    sub.bytes.size(), needed));
    return cc::unit{};
}

/// Calls `fn(block, block_x, block_y, slice)` for every block of a block-compressed subresource.
/// `block` points at the block's first byte; check_subresource has made sure every block lies inside `sub.bytes`.
template <class Fn>
void for_each_block(data const& tex, subresource const& sub, Fn&& fn)
{
    auto const block_bytes = tex.row_pitch(4); // one block
    auto const blocks_x = (sub.width + 3) / 4;
    auto const blocks_y = (sub.height + 3) / 4;
    auto const* const bytes = sub.bytes.data();
    auto offset = isize(0);
    for (auto z = 0; z < sub.depth; ++z)
        for (auto by = 0; by < blocks_y; ++by)
            for (auto bx = 0; bx < blocks_x; ++bx)
            {
                fn(bytes + offset, bx, by, z);
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

bcn::rgba8_block decode_block_rgba8(pixel_format fmt, byte const* block)
{
    switch (fmt)
    {
    case pixel_format::bc1_unorm:
    case pixel_format::bc1_unorm_srgb:
        return bcn::decode_bc1(bcn::small_block(block));
    case pixel_format::bc2_unorm:
    case pixel_format::bc2_unorm_srgb:
        return bcn::decode_bc2(bcn::large_block(block));
    case pixel_format::bc3_unorm:
    case pixel_format::bc3_unorm_srgb:
        return bcn::decode_bc3(bcn::large_block(block));
    case pixel_format::bc4_unorm:
    case pixel_format::bc4_snorm:
        return bcn::decode_bc4(bcn::small_block(block), fmt == pixel_format::bc4_snorm);
    case pixel_format::bc5_unorm:
    case pixel_format::bc5_snorm:
        return bcn::decode_bc5(bcn::large_block(block), fmt == pixel_format::bc5_snorm);
    default:
        return bcn::decode_bc7(bcn::large_block(block));
    }
}

/// BC7's interpolation is integer-specified, so its 8-bit result is exact and only widens here.
bcn::rgba32f_block decode_block_rgba32f(pixel_format fmt, byte const* block)
{
    switch (fmt)
    {
    case pixel_format::bc1_unorm:
    case pixel_format::bc1_unorm_srgb:
        return bcn::decode_bc1_f32(bcn::small_block(block));
    case pixel_format::bc2_unorm:
    case pixel_format::bc2_unorm_srgb:
        return bcn::decode_bc2_f32(bcn::large_block(block));
    case pixel_format::bc3_unorm:
    case pixel_format::bc3_unorm_srgb:
        return bcn::decode_bc3_f32(bcn::large_block(block));
    case pixel_format::bc4_unorm:
    case pixel_format::bc4_snorm:
        return bcn::decode_bc4_f32(bcn::small_block(block), fmt == pixel_format::bc4_snorm);
    case pixel_format::bc5_unorm:
    case pixel_format::bc5_snorm:
        return bcn::decode_bc5_f32(bcn::large_block(block), fmt == pixel_format::bc5_snorm);
    case pixel_format::bc6h_ufloat:
    case pixel_format::bc6h_sfloat:
        return bcn::decode_bc6h(bcn::large_block(block), fmt == pixel_format::bc6h_sfloat);
    default:
    {
        auto const texels = bcn::decode_bc7(bcn::large_block(block));
        auto out = bcn::rgba32f_block{};
        for (auto i = 0; i < 64; ++i)
            out.rgba[i] = float(texels.rgba[i]) / 255.0f;
        return out;
    }
    }
}
} // namespace

// decoding
// -------------------------------------------------------------------------------------------------

cc::result<cc::vector<byte>> decode_rgba8(data const& tex, subresource const& sub, decode_options opts)
{
    CC_RECORD_SCOPE("dds.decode_rgba8");

    if (is_float(tex.format))
        return cc::error(cc::format("dds decode: {} holds floats; decode it with decode_rgba32f", to_string(tex.format)));
    CC_RETURN_IF_ERROR(check_subresource(tex, sub, 1, opts));

    auto const texel_count = isize(sub.width) * sub.height * sub.depth;
    auto out = cc::vector<byte>::create_uninitialized(texel_count * 4);
    auto* const dst = reinterpret_cast<u8*>(out.data());

    if (is_block_compressed(tex.format))
    {
        for_each_block(tex, sub,
                       [&](byte const* block, int bx, int by, int z)
                       {
                           auto const texels = decode_block_rgba8(tex.format, block);
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
                    auto const rgba
                        = decode_masked_texel(tex.file_header.pixels, load_masked_texel(src, at, bytes_per_texel));
                    for (auto c = 0; c < 4; ++c)
                        o[c] = to_unorm8(rgba[c]);
                    break;
                }
                }
            }
        }

    return cc::move(out);
}

cc::result<cc::vector<float>> decode_rgba32f(data const& tex, subresource const& sub, decode_options opts)
{
    CC_RETURN_IF_ERROR(check_subresource(tex, sub, isize(sizeof(float)), opts));
    auto out = cc::vector<float>::create_uninitialized(isize(sub.width) * sub.height * sub.depth * 4);
    CC_RETURN_IF_ERROR(decode_rgba32f_into(tex, sub, cc::span<float>(out), opts));
    return cc::move(out);
}

cc::result<cc::unit> decode_rgba32f_into(data const& tex, subresource const& sub, cc::span<float> out, decode_options opts)
{
    CC_RECORD_SCOPE("dds.decode_rgba32f");
    CC_RETURN_IF_ERROR(check_subresource(tex, sub, isize(sizeof(float)), opts));

    auto const texel_count = isize(sub.width) * sub.height * sub.depth;
    CC_ASSERT(out.size() == texel_count * 4, "the output must hold four floats per texel of the subresource");
    auto const fmt = tex.format;

    if (is_block_compressed(fmt))
    {
        for_each_block(tex, sub,
                       [&](byte const* block, int bx, int by, int z)
                       {
                           auto const texels = decode_block_rgba32f(fmt, block);
                           store_block<float>(cc::span<float const>(texels.rgba), sub, bx, by, z, out.data());
                       });
        return cc::unit{};
    }

    if (fmt == pixel_format::masked)
    {
        auto const src = sub.bytes.span();
        auto const bytes_per_texel = tex.row_pitch(1);
        auto const row_pitch = tex.row_pitch(sub.width);
        auto const slice_size = tex.slice_byte_size(sub.width, sub.height);
        for (auto z = 0; z < sub.depth; ++z)
            for (auto y = 0; y < sub.height; ++y)
                for (auto x = 0; x < sub.width; ++x)
                {
                    auto const at = isize(z) * slice_size + isize(y) * row_pitch + isize(x) * bytes_per_texel;
                    auto const rgba
                        = decode_masked_texel(tex.file_header.pixels, load_masked_texel(src, at, bytes_per_texel));
                    auto const dst = ((isize(z) * sub.height + y) * sub.width + x) * 4;
                    for (auto c = 0; c < 4; ++c)
                        out[dst + c] = rgba[c];
                }
        return cc::unit{};
    }

    if (is_float(fmt))
    {
        // r16 / rgba16 / r32 / rgba32 floats: a missing channel is 0, and a missing alpha 1.
        auto const src = sub.bytes.span();
        auto const channels = fmt == pixel_format::r16_float || fmt == pixel_format::r32_float ? 1 : 4;
        auto const is_half = fmt == pixel_format::r16_float || fmt == pixel_format::rgba16_float;
        auto const sample_bytes = isize(is_half ? 2 : 4);
        for (auto t = isize(0); t < texel_count; ++t)
        {
            auto rgba = cc::fixed_array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
            for (auto c = 0; c < channels; ++c)
            {
                auto const at = (t * channels + c) * sample_bytes;
                rgba[c] = is_half ? float(tg::f16::make_from_bits(cc::load_bytes_le<u16>(src, at)))
                                  : cc::load_bytes_le<float>(src, at);
            }
            for (auto c = 0; c < 4; ++c)
                out[t * 4 + c] = rgba[c];
        }
        return cc::unit{};
    }

    // What is left stores 8-bit channels as they are, so the 8-bit decode is exact and only widens here.
    auto bytes = decode_rgba8(tex, sub, opts);
    CC_RETURN_IF_ERROR(bytes);
    for (auto i = isize(0); i < texel_count * 4; ++i)
        out[i] = float(u8(bytes.value()[i])) / 255.0f;
    return cc::unit{};
}
} // namespace babel::dds
