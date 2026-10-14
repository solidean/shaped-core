#include <babel-serializer/image/dds.hh>
#include <babel-serializer/image/impl/dds_flags.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>

namespace babel::dds
{
namespace
{
using namespace babel::dds::impl;

constexpr u32 four_cc(char a, char b, char c, char d)
{
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

constexpr u32 magic = four_cc('D', 'D', 'S', ' ');
constexpr isize legacy_header_end = 128;
constexpr isize dx10_header_end = 148;

// Header flags, as the format names them.
constexpr u32 flag_depth = 0x800000;
constexpr u32 caps2_cubemap = 0x200;
constexpr u32 misc_texture_cube = 0x4;

// D3D's numeric FourCCs, which legacy writers use for the float formats.
constexpr u32 d3dfmt_r16f = 111;
constexpr u32 d3dfmt_a16b16g16r16f = 113;
constexpr u32 d3dfmt_r32f = 114;
constexpr u32 d3dfmt_a32b32g32r32f = 116;

// An array may not have more layers than this, far past what any API creates.
constexpr u32 max_array_size = 1u << 16;

struct resolved_format
{
    pixel_format format = pixel_format::rgba8_unorm;
    alpha_mode alpha = alpha_mode::unknown;
};

cc::optional<pixel_format> from_dxgi(u32 dxgi)
{
    // The TYPELESS variants (70, 73, 76, 79, 82, 94, 97) read as their UNORM sibling.
    switch (dxgi)
    {
    case 2:
        return pixel_format::rgba32_float;
    case 10:
        return pixel_format::rgba16_float;
    case 28:
        return pixel_format::rgba8_unorm;
    case 29:
        return pixel_format::rgba8_unorm_srgb;
    case 41:
        return pixel_format::r32_float;
    case 49:
        return pixel_format::rg8_unorm;
    case 54:
        return pixel_format::r16_float;
    case 61:
        return pixel_format::r8_unorm;
    case 70:
    case 71:
        return pixel_format::bc1_unorm;
    case 72:
        return pixel_format::bc1_unorm_srgb;
    case 73:
    case 74:
        return pixel_format::bc2_unorm;
    case 75:
        return pixel_format::bc2_unorm_srgb;
    case 76:
    case 77:
        return pixel_format::bc3_unorm;
    case 78:
        return pixel_format::bc3_unorm_srgb;
    case 79:
    case 80:
        return pixel_format::bc4_unorm;
    case 81:
        return pixel_format::bc4_snorm;
    case 82:
    case 83:
        return pixel_format::bc5_unorm;
    case 84:
        return pixel_format::bc5_snorm;
    case 87:
        return pixel_format::bgra8_unorm;
    case 88:
        return pixel_format::bgrx8_unorm;
    case 91:
        return pixel_format::bgra8_unorm_srgb;
    case 93:
        return pixel_format::bgrx8_unorm_srgb;
    case 94:
    case 95:
        return pixel_format::bc6h_ufloat;
    case 96:
        return pixel_format::bc6h_sfloat;
    case 97:
    case 98:
        return pixel_format::bc7_unorm;
    case 99:
        return pixel_format::bc7_unorm_srgb;
    default:
        return {};
    }
}

cc::optional<resolved_format> from_four_cc(u32 code)
{
    if (code == four_cc('D', 'X', 'T', '1'))
        return resolved_format{.format = pixel_format::bc1_unorm};
    if (code == four_cc('D', 'X', 'T', '2'))
        return resolved_format{.format = pixel_format::bc2_unorm, .alpha = alpha_mode::premultiplied};
    if (code == four_cc('D', 'X', 'T', '3'))
        return resolved_format{.format = pixel_format::bc2_unorm};
    if (code == four_cc('D', 'X', 'T', '4'))
        return resolved_format{.format = pixel_format::bc3_unorm, .alpha = alpha_mode::premultiplied};
    if (code == four_cc('D', 'X', 'T', '5'))
        return resolved_format{.format = pixel_format::bc3_unorm};
    if (code == four_cc('A', 'T', 'I', '1') || code == four_cc('B', 'C', '4', 'U'))
        return resolved_format{.format = pixel_format::bc4_unorm};
    if (code == four_cc('B', 'C', '4', 'S'))
        return resolved_format{.format = pixel_format::bc4_snorm};
    if (code == four_cc('A', 'T', 'I', '2') || code == four_cc('B', 'C', '5', 'U'))
        return resolved_format{.format = pixel_format::bc5_unorm};
    if (code == four_cc('B', 'C', '5', 'S'))
        return resolved_format{.format = pixel_format::bc5_snorm};
    if (code == d3dfmt_r16f)
        return resolved_format{.format = pixel_format::r16_float};
    if (code == d3dfmt_a16b16g16r16f)
        return resolved_format{.format = pixel_format::rgba16_float};
    if (code == d3dfmt_r32f)
        return resolved_format{.format = pixel_format::r32_float};
    if (code == d3dfmt_a32b32g32r32f)
        return resolved_format{.format = pixel_format::rgba32_float};
    return {};
}

/// A FourCC as text when it is printable, and as its number otherwise.
cc::string describe_four_cc(u32 code)
{
    auto text = cc::string();
    for (auto i = 0; i < 4; ++i)
    {
        auto const c = char((code >> (8 * i)) & 0xFFu);
        if (c < ' ' || c > '~')
            return cc::format("{}", code);
        text.push_back(c);
    }
    return cc::format("'{}'", text);
}

/// True for an empty mask or one run of set bits: a channel's value is the bits it covers, read as one integer.
bool is_contiguous_mask(u32 mask)
{
    if (mask == 0)
        return true;
    auto const run = u64(mask >> cc::count_trailing_zeroes(mask));
    return (run & (run + 1)) == 0;
}

cc::result<resolved_format> resolve_masked(pixel_format_header const& pf)
{
    if ((pf.flags & (pf_rgb | pf_luminance | pf_alpha)) == 0)
        return cc::error(cc::format(
            "dds: the pixel format names no FourCC, RGB, luminance or alpha layout (flags {:#x})", pf.flags));

    auto const bits = pf.rgb_bit_count;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return cc::error(cc::format("dds: a masked pixel format of {} bits per pixel is not one babel reads", bits));

    auto const in_range = bits == 32 ? ~u32(0) : (u32(1) << bits) - 1;
    auto const a = effective_alpha_mask(pf);
    if (((pf.r_mask | pf.g_mask | pf.b_mask | a) & ~in_range) != 0)
        return cc::error(cc::format("dds: a channel mask reaches past the {}-bit pixel", bits));
    if ((pf.r_mask | pf.g_mask | pf.b_mask | a) == 0)
        return cc::error("dds: a masked pixel format with every mask zero has no channels");
    auto const masks = cc::fixed_array<u32, 4>{pf.r_mask, pf.g_mask, pf.b_mask, a};
    for (auto const mask : masks)
        if (!is_contiguous_mask(mask))
            return cc::error(cc::format("dds: channel mask {:#x} is not one run of bits", mask));

    // The layouts a DXGI format names map to it, so a consumer can upload them as they are.
    if ((pf.flags & pf_rgb) != 0)
    {
        if (bits == 32 && pf.r_mask == 0x00FF0000 && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x000000FF)
        {
            if (a == 0xFF000000)
                return resolved_format{.format = pixel_format::bgra8_unorm};
            if (a == 0)
                return resolved_format{.format = pixel_format::bgrx8_unorm};
        }
        if (bits == 32 && pf.r_mask == 0x000000FF && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x00FF0000 && a == 0xFF000000)
            return resolved_format{.format = pixel_format::rgba8_unorm};
        if (bits == 16 && pf.r_mask == 0x00FF && pf.g_mask == 0xFF00 && pf.b_mask == 0 && a == 0)
            return resolved_format{.format = pixel_format::rg8_unorm};
        if (bits == 8 && pf.r_mask == 0xFF && pf.g_mask == 0 && pf.b_mask == 0 && a == 0)
            return resolved_format{.format = pixel_format::r8_unorm};
    }
    return resolved_format{.format = pixel_format::masked};
}

int block_bytes_of(pixel_format fmt)
{
    switch (fmt)
    {
    case pixel_format::bc1_unorm:
    case pixel_format::bc1_unorm_srgb:
    case pixel_format::bc4_unorm:
    case pixel_format::bc4_snorm:
        return 8;
    case pixel_format::bc2_unorm:
    case pixel_format::bc2_unorm_srgb:
    case pixel_format::bc3_unorm:
    case pixel_format::bc3_unorm_srgb:
    case pixel_format::bc5_unorm:
    case pixel_format::bc5_snorm:
    case pixel_format::bc6h_ufloat:
    case pixel_format::bc6h_sfloat:
    case pixel_format::bc7_unorm:
    case pixel_format::bc7_unorm_srgb:
        return 16;
    default:
        return 0;
    }
}

/// Bits per texel of an uncompressed format; `masked_bits` is the header's count, which a masked format uses.
int bits_per_texel(pixel_format fmt, u32 masked_bits)
{
    switch (fmt)
    {
    case pixel_format::rgba8_unorm:
    case pixel_format::rgba8_unorm_srgb:
    case pixel_format::bgra8_unorm:
    case pixel_format::bgra8_unorm_srgb:
    case pixel_format::bgrx8_unorm:
    case pixel_format::bgrx8_unorm_srgb:
    case pixel_format::r32_float:
        return 32;
    case pixel_format::r8_unorm:
        return 8;
    case pixel_format::rg8_unorm:
    case pixel_format::r16_float:
        return 16;
    case pixel_format::rgba16_float:
        return 64;
    case pixel_format::rgba32_float:
        return 128;
    case pixel_format::masked:
        return int(masked_bits);
    default:
        return 0;
    }
}

/// The number of levels a full chain down to 1x1x1 has.
int full_chain_length(u32 width, u32 height, u32 depth)
{
    auto largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    return int(cc::bit_width(largest));
}

} // namespace

// queries
// -------------------------------------------------------------------------------------------------

bool is_block_compressed(pixel_format fmt)
{
    return block_bytes_of(fmt) > 0;
}

bool is_float(pixel_format fmt)
{
    switch (fmt)
    {
    case pixel_format::bc6h_ufloat:
    case pixel_format::bc6h_sfloat:
    case pixel_format::r16_float:
    case pixel_format::rgba16_float:
    case pixel_format::r32_float:
    case pixel_format::rgba32_float:
        return true;
    default:
        return false;
    }
}

bool is_srgb(pixel_format fmt)
{
    switch (fmt)
    {
    case pixel_format::bc1_unorm_srgb:
    case pixel_format::bc2_unorm_srgb:
    case pixel_format::bc3_unorm_srgb:
    case pixel_format::bc7_unorm_srgb:
    case pixel_format::rgba8_unorm_srgb:
    case pixel_format::bgra8_unorm_srgb:
    case pixel_format::bgrx8_unorm_srgb:
        return true;
    default:
        return false;
    }
}

cc::string_view to_string(pixel_format fmt)
{
    switch (fmt)
    {
    case pixel_format::bc1_unorm:
        return "bc1_unorm";
    case pixel_format::bc1_unorm_srgb:
        return "bc1_unorm_srgb";
    case pixel_format::bc2_unorm:
        return "bc2_unorm";
    case pixel_format::bc2_unorm_srgb:
        return "bc2_unorm_srgb";
    case pixel_format::bc3_unorm:
        return "bc3_unorm";
    case pixel_format::bc3_unorm_srgb:
        return "bc3_unorm_srgb";
    case pixel_format::bc4_unorm:
        return "bc4_unorm";
    case pixel_format::bc4_snorm:
        return "bc4_snorm";
    case pixel_format::bc5_unorm:
        return "bc5_unorm";
    case pixel_format::bc5_snorm:
        return "bc5_snorm";
    case pixel_format::bc6h_ufloat:
        return "bc6h_ufloat";
    case pixel_format::bc6h_sfloat:
        return "bc6h_sfloat";
    case pixel_format::bc7_unorm:
        return "bc7_unorm";
    case pixel_format::bc7_unorm_srgb:
        return "bc7_unorm_srgb";
    case pixel_format::rgba8_unorm:
        return "rgba8_unorm";
    case pixel_format::rgba8_unorm_srgb:
        return "rgba8_unorm_srgb";
    case pixel_format::bgra8_unorm:
        return "bgra8_unorm";
    case pixel_format::bgra8_unorm_srgb:
        return "bgra8_unorm_srgb";
    case pixel_format::bgrx8_unorm:
        return "bgrx8_unorm";
    case pixel_format::bgrx8_unorm_srgb:
        return "bgrx8_unorm_srgb";
    case pixel_format::r8_unorm:
        return "r8_unorm";
    case pixel_format::rg8_unorm:
        return "rg8_unorm";
    case pixel_format::r16_float:
        return "r16_float";
    case pixel_format::rgba16_float:
        return "rgba16_float";
    case pixel_format::r32_float:
        return "r32_float";
    case pixel_format::rgba32_float:
        return "rgba32_float";
    case pixel_format::masked:
        return "masked";
    }
    return "?";
}

int data::face_count() const
{
    return is_cube ? cc::popcount(u32(cube_faces)) : 1;
}

subresource const* data::find(int layer, int face, int mip) const
{
    for (auto const& s : subresources)
        if (s.layer == layer && s.face == face && s.mip == mip)
            return &s;
    return nullptr;
}

isize data::row_pitch(int row_width) const
{
    if (auto const block_bytes = block_bytes_of(format); block_bytes > 0)
        return (isize(row_width) + 3) / 4 * block_bytes;
    return (isize(row_width) * bits_per_texel(format, file_header.pixels.rgb_bit_count) + 7) / 8;
}

isize data::slice_byte_size(int slice_width, int slice_height) const
{
    if (is_block_compressed(format))
        return row_pitch(slice_width) * ((isize(slice_height) + 3) / 4);
    return row_pitch(slice_width) * isize(slice_height);
}

// reading
// -------------------------------------------------------------------------------------------------

cc::result<data> read(cc::pinned_data<byte const> const& bytes)
{
    CC_RECORD_SCOPE("dds.read");

    auto const in = bytes.span();
    if (in.size() < legacy_header_end)
        return cc::error(cc::format("dds: {} bytes cannot hold the {}-byte header", in.size(), legacy_header_end));
    if (cc::load_bytes_le<u32>(in, 0) != magic)
        return cc::error("dds: missing the 'DDS ' magic");
    if (auto const size = cc::load_bytes_le<u32>(in, 4); size != 124)
        return cc::error(cc::format("dds: header size is {}, where the format fixes it at 124", size));
    if (auto const size = cc::load_bytes_le<u32>(in, 76); size != 32)
        return cc::error(cc::format("dds: pixel-format size is {}, where the format fixes it at 32", size));

    auto result = data{};
    auto& h = result.file_header;
    h.flags = cc::load_bytes_le<u32>(in, 8);
    h.height = cc::load_bytes_le<u32>(in, 12);
    h.width = cc::load_bytes_le<u32>(in, 16);
    h.pitch_or_linear_size = cc::load_bytes_le<u32>(in, 20);
    h.depth = cc::load_bytes_le<u32>(in, 24);
    h.mip_map_count = cc::load_bytes_le<u32>(in, 28);
    h.pixels = {.flags = cc::load_bytes_le<u32>(in, 80),
                .four_cc = cc::load_bytes_le<u32>(in, 84),
                .rgb_bit_count = cc::load_bytes_le<u32>(in, 88),
                .r_mask = cc::load_bytes_le<u32>(in, 92),
                .g_mask = cc::load_bytes_le<u32>(in, 96),
                .b_mask = cc::load_bytes_le<u32>(in, 100),
                .a_mask = cc::load_bytes_le<u32>(in, 104)};
    h.caps = cc::load_bytes_le<u32>(in, 108);
    h.caps2 = cc::load_bytes_le<u32>(in, 112);

    auto const uses_four_cc = (h.pixels.flags & pf_four_cc) != 0;
    h.has_dx10 = uses_four_cc && h.pixels.four_cc == four_cc('D', 'X', '1', '0');

    auto first_byte = legacy_header_end;
    auto depth = u32(1);
    auto array_size = u32(1);

    if (h.has_dx10)
    {
        if (in.size() < dx10_header_end)
            return cc::error(cc::format("dds: {} bytes cannot hold the {}-byte DX10 header", in.size(), dx10_header_end));
        h.dxgi_format = cc::load_bytes_le<u32>(in, 128);
        h.resource_dimension = cc::load_bytes_le<u32>(in, 132);
        h.misc_flag = cc::load_bytes_le<u32>(in, 136);
        h.array_size = cc::load_bytes_le<u32>(in, 140);
        h.misc_flags2 = cc::load_bytes_le<u32>(in, 144);
        first_byte = dx10_header_end;

        auto const fmt = from_dxgi(h.dxgi_format);
        if (!fmt.has_value())
            return cc::error(cc::format("dds: DXGI format {} is not one babel reads", h.dxgi_format));
        result.format = fmt.value();

        auto const alpha = h.misc_flags2 & 0x7u;
        if (alpha > 4)
            return cc::error(cc::format("dds: alpha mode {} is not one the DX10 header defines", alpha));
        result.alpha = alpha_mode(alpha);

        switch (h.resource_dimension)
        {
        case 2:
            result.shape = dimension::texture_1d;
            break;
        case 3:
            result.shape = dimension::texture_2d;
            break;
        case 4:
            result.shape = dimension::texture_3d;
            break;
        default:
            return cc::error(cc::format("dds: resource dimension {} is not a texture", h.resource_dimension));
        }

        if ((h.misc_flag & misc_texture_cube) != 0)
        {
            if (result.shape != dimension::texture_2d)
                return cc::error("dds: only a 2D texture can be a cube map");
            result.is_cube = true;
            result.cube_faces = 0x3F;
        }

        array_size = h.array_size;
        if (array_size == 0)
            return cc::error("dds: the DX10 header declares an array of zero layers");
        if (result.shape == dimension::texture_3d)
        {
            if (array_size != 1)
                return cc::error("dds: a volume texture cannot be an array");
            depth = h.depth;
        }
        if (result.shape == dimension::texture_1d && h.height > 1)
            return cc::error(cc::format("dds: a 1D texture of height {}", h.height));
    }
    else
    {
        if (uses_four_cc)
        {
            auto const fmt = from_four_cc(h.pixels.four_cc);
            if (!fmt.has_value())
                return cc::error(cc::format("dds: FourCC {} is not one babel reads", describe_four_cc(h.pixels.four_cc)));
            result.format = fmt.value().format;
            result.alpha = fmt.value().alpha;
        }
        else
        {
            auto fmt = resolve_masked(h.pixels);
            CC_RETURN_IF_ERROR(fmt);
            result.format = fmt.value().format;
        }

        // DDSD_DEPTH alone makes a volume, as DirectXTex reads it; DDSCAPS2_VOLUME without it says nothing about depth.
        if ((h.flags & flag_depth) != 0)
        {
            result.shape = dimension::texture_3d;
            depth = h.depth;
        }
        else if ((h.caps2 & caps2_cubemap) != 0)
        {
            result.is_cube = true;
            result.cube_faces = u8((h.caps2 >> 10) & 0x3Fu);
            if (result.cube_faces == 0)
                return cc::error("dds: a cube map that stores none of its faces");
        }
    }

    // A 1D texture may leave its height at 0, and every other one has to name all its extents.
    auto const height = result.shape == dimension::texture_1d && h.height == 0 ? u32(1) : h.height;
    if (h.width == 0 || height == 0 || depth == 0)
        return cc::error(cc::format("dds: zero extent {}x{}x{}", h.width, height, depth));
    if (h.width > max_extent || height > max_extent || depth > max_extent)
        return cc::error(cc::format("dds: implausible extent {}x{}x{}", h.width, height, depth));
    if (array_size > max_array_size)
        return cc::error(cc::format("dds: implausible array of {} layers", array_size));
    if (result.is_cube && h.width != height)
        return cc::error(cc::format("dds: a cube map's faces must be square, not {}x{}", h.width, height));

    // A count of 0 means the base level alone; so does a header that leaves the count's flag unset yet fills it in,
    // which writers do, and which every reader takes at its word.
    // The count is compared as stored, so a huge u32 cannot wrap into a negative int on the way.
    auto const stored_mips = h.mip_map_count == 0 ? u32(1) : h.mip_map_count;
    auto const longest = full_chain_length(h.width, height, depth);
    if (stored_mips > u32(longest))
        return cc::error(cc::format("dds: {} mip levels, where a {}x{}x{} texture has at most {}", //
                                    stored_mips, h.width, height, depth, longest));
    auto const mips = int(stored_mips);

    result.width = int(h.width);
    result.height = int(height);
    result.depth = int(depth);
    result.mip_count = mips;
    result.array_size = int(array_size);

    auto offset = first_byte;
    for (auto layer = 0; layer < result.array_size; ++layer)
        for (auto face = 0; face < (result.is_cube ? 6 : 1); ++face)
        {
            if (result.is_cube && (result.cube_faces & (1u << face)) == 0)
                continue;
            for (auto mip = 0; mip < mips; ++mip)
            {
                auto const w = mip_extent(result.width, mip);
                auto const hgt = mip_extent(result.height, mip);
                auto const d = mip_extent(result.depth, mip);
                auto const size = result.slice_byte_size(w, hgt) * isize(d);
                if (size > in.size() - offset)
                    return cc::error(cc::format("dds: truncated — layer {} face {} mip {} needs bytes {}..{}, " //
                                                "and the file ends at {}",
                                                layer, face, mip, offset, offset + size, in.size()));

                result.subresources.push_back({.layer = layer,
                                               .face = face,
                                               .mip = mip,
                                               .width = w,
                                               .height = hgt,
                                               .depth = d,
                                               .bytes = bytes.subdata({.offset = offset, .size = size})});
                offset += size;
            }
        }

    CC_RECORD_ACCUM("dds.bytes_read", cc::rec::unit_bytes, offset);
    return cc::move(result);
}

cc::result<data> read(cc::read_stream& in)
{
    auto slurped = in.read_all();
    CC_RETURN_IF_ERROR(slurped);
    // make_pinned_data moves the slurped vector into the pin, so the read is the only copy.
    return read(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(slurped).value())));
}

cc::result<data> read(cc::span<byte const> bytes)
{
    return read(cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytes)));
}
} // namespace babel::dds
