#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string_view.hh>

// Low-level DDS (DirectDraw Surface) reader (image/).
//
// The format-shaped view of a `.dds`: the header as stored, the pixel format it resolves to, and every subresource
// — each array layer, cube face and mip level — as a zero-copy view of the input.
// Nothing is decoded on read, because the bytes are already what a GPU uploads: block-compressed formats stay blocks.
// decode_rgba8 / decode_rgba32f turn one subresource into plain texels; babel::image sits on top and keeps mip 0.
//
// Native, with no backend: the container is a fixed header, and the BCn block decoders are babel::bcn.
//
// THE FORMAT IS RESOLVED ONCE, WHICHEVER HEADER SPELLED IT.
// A legacy file names its format by FourCC (`DXT1`, `ATI2`, ...) or by bit masks, and a DX10 file by DXGI number.
// `format` is the one answer, and `file_header` keeps how the file said it.
// DXT2 and DXT4 are BC2 and BC3 with premultiplied alpha, so they resolve to those, with `alpha` saying so.
// ATI1 / BC4U / BC4S and ATI2 / BC5U / BC5S resolve to BC4 and BC5, the first BC5 block being red (x).
//
// A malformed file is an error, never a partial read.
// The header must be self-consistent, and every subresource it implies must lie inside the input.
// Bytes past the last subresource are tolerated.
//
// ROWS ARE READ TIGHTLY PACKED, and the header's pitch / linear size is ignored.
// The extent and the format alone decide each level's size, as DirectXTex reads a file by default.
// A legacy writer that padded rows to a DWORD therefore reads as tight rows, and its padding shifts the texels.
//
//   auto const tex = babel::dds::read(cc::make_pinned_data(cc::move(file_bytes))).value();
//   auto const& top = tex.subresources.front();         // never empty after a successful read
//   auto const texels = babel::dds::decode_rgba8(tex, top).value();

/// What the texels of a DDS file are, as a GPU names them.
/// The DXGI spelling, because that is the vocabulary both header generations map into.
enum class babel::dds::pixel_format : babel::u8
{
    // Block-compressed, 4x4 texels per block.
    bc1_unorm, // DXT1
    bc1_unorm_srgb,
    bc2_unorm, // DXT2 (premultiplied) / DXT3
    bc2_unorm_srgb,
    bc3_unorm, // DXT4 (premultiplied) / DXT5
    bc3_unorm_srgb,
    bc4_unorm, // ATI1 / BC4U
    bc4_snorm, // BC4S
    bc5_unorm, // ATI2 / BC5U
    bc5_snorm, // BC5S
    bc6h_ufloat,
    bc6h_sfloat,
    bc7_unorm,
    bc7_unorm_srgb,

    // Uncompressed, named in memory byte order.
    rgba8_unorm,
    rgba8_unorm_srgb,
    bgra8_unorm,
    bgra8_unorm_srgb,
    bgrx8_unorm, // the fourth byte is padding, and decodes as opaque
    bgrx8_unorm_srgb,
    r8_unorm,
    rg8_unorm,
    r16_float,
    rgba16_float,
    r32_float,
    rgba32_float,

    /// A legacy layout described by the header's bit masks alone, with no DXGI name.
    /// `file_header.pixels` holds the masks and the bit count, and its flags say whether they are RGB, luminance or alpha.
    /// The masks are read as written: DirectXTex swaps red and blue in a 10:10:10:2 file by default, for an old D3DX
    /// writer that labeled them the wrong way round, and babel does not.
    masked,
};

/// How the texture's alpha is meant to be read.
/// A legacy header can say only `premultiplied` (DXT2 / DXT4); a DX10 header says it in its misc flags.
enum class babel::dds::alpha_mode : babel::u8
{
    unknown,
    straight,
    premultiplied,
    opaque,
    custom,
};

/// The texture's shape, from the DX10 header or the legacy caps.
enum class babel::dds::dimension : babel::u8
{
    texture_1d,
    texture_2d, // cube maps included; `data::is_cube` tells them apart
    texture_3d,
};

/// The legacy pixel-format block, as stored.
struct babel::dds::pixel_format_header
{
    u32 flags = 0;
    u32 four_cc = 0;
    u32 rgb_bit_count = 0;
    u32 r_mask = 0;
    u32 g_mask = 0;
    u32 b_mask = 0;
    u32 a_mask = 0;
};

/// The header fields as stored, legacy and DX10 alike.
/// The DX10 fields are zero when the file has no DX10 header.
struct babel::dds::header
{
    u32 flags = 0;
    u32 height = 0;
    u32 width = 0;
    u32 pitch_or_linear_size = 0; // as stored, and ignored: rows are read tightly packed
    u32 depth = 0;
    u32 mip_map_count = 0;
    pixel_format_header pixels;
    u32 caps = 0;
    u32 caps2 = 0;

    bool has_dx10 = false;
    u32 dxgi_format = 0;
    u32 resource_dimension = 0;
    u32 misc_flag = 0;
    u32 array_size = 0;
    u32 misc_flags2 = 0;
};

/// One layer, face and mip of the texture, and its bytes as the file stores them.
struct babel::dds::subresource
{
    int layer = 0; // the array element; 0 for a texture that is not an array
    int face = 0;  // the cube face, 0..5 for +X -X +Y -Y +Z -Z; 0 for a texture that is not a cube
    int mip = 0;

    int width = 0;
    int height = 0;
    int depth = 1; // slices of a volume level, stored one after another

    /// The level's bytes, a view sharing the input's owner: blocks for a compressed format, rows otherwise.
    /// Rows are tightly packed, so a row is `data::row_pitch(width)` bytes.
    cc::pinned_data<byte const> bytes;
};

/// A faithful read of a DDS file.
/// Read-once.
struct babel::dds::data
{
    header file_header;

    pixel_format format = pixel_format::rgba8_unorm;
    alpha_mode alpha = alpha_mode::unknown;
    dimension shape = dimension::texture_2d;

    int width = 0;
    int height = 0;
    int depth = 1;
    int mip_count = 1;
    int array_size = 1; // array layers; a cube array counts cubes, not faces

    bool is_cube = false;
    u8 cube_faces = 0; // bit f set when face f is present; a legacy cube map may store fewer than six

    /// Every subresource in file order: layer by layer, each face of a layer, then each mip of a face.
    cc::vector<subresource> subresources;

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }

    /// The faces each layer has: the present ones of a cube map, 1 otherwise.
    [[nodiscard]] int face_count() const;

    /// The subresource at that position, or nullptr where the texture has none — a partial cube map may lack face 0.
    /// A linear scan over `subresources`.
    [[nodiscard]] subresource const* find(int layer, int face, int mip) const;

    /// The bytes one row of a `width`-texel level occupies — a row of blocks for a compressed format.
    [[nodiscard]] isize row_pitch(int width) const;

    /// The bytes one `width` x `height` slice occupies.
    [[nodiscard]] isize slice_byte_size(int width, int height) const;
};

namespace babel::dds
{
/// The extent of mip `level` along an axis whose base is `base`, never below 1.
/// `level` must be in 0..31, since it is a shift of an `int`.
[[nodiscard]] constexpr int mip_extent(int base, int level)
{
    auto const e = base >> level;
    return e < 1 ? 1 : e;
}

/// True for the BC1 - BC7 formats, which store 4x4 blocks rather than texels.
[[nodiscard]] bool is_block_compressed(pixel_format fmt);

/// True for the formats whose samples are floats: BC6H and the 16- and 32-bit float ones.
/// decode_rgba8 refuses them, and decode_rgba32f is the decode that keeps their range.
[[nodiscard]] bool is_float(pixel_format fmt);

/// True for the sRGB-encoded formats.
[[nodiscard]] bool is_srgb(pixel_format fmt);

/// The format's name, for diagnostics.
[[nodiscard]] cc::string_view to_string(pixel_format fmt);

// reading
// -------------------------------------------------------------------------------------------------

/// Read a whole DDS file.
/// This is the zero-copy entry point: every subresource's `bytes` is a view of `bytes` sharing its owner.
/// Errors on a missing magic, a header that contradicts itself, a format it cannot name, or a file too short for the subresources its header describes.
/// Every subresource covers at least one input byte, so its records stay within `sizeof(subresource)` times the input's size.
/// The worst case is a 65536-layer cube array of 1 x 1 8-bit texels.
[[nodiscard]] cc::result<data> read(cc::pinned_data<byte const> const& bytes);

/// Convenience: slurps the stream, then pins the slurped buffer (moved into the pin, not copied).
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

/// Convenience: COPIES the input into an owned pin first, so the subresources stay valid independently of the caller's memory.
/// Pass a cc::pinned_data to avoid that copy.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

} // namespace babel::dds

/// Decode knobs.
struct babel::dds::decode_options
{
    /// The most bytes a decoded subresource may take; one that would take more is an error before anything is allocated.
    /// A block-compressed texture decodes to 4 to 32 times its size, so this bounds what a large file can demand.
    /// The default holds an 8192 x 8192 decode to rgba8, but only 4096 x 4096 to rgba32f.
    /// A larger float texture, such as an 8192 x 4096 BC6H environment map, needs it raised.
    /// The largest `isize` disables the check.
    isize max_decoded_bytes = isize(1) << 28;
};

namespace babel::dds
{
// decoding
// -------------------------------------------------------------------------------------------------

/// Decode one subresource of `tex` to 8-bit r, g, b, a texels, row-major and top-left origin, slice after slice.
/// Errors for a float format, which decode_rgba32f handles.
/// Channels a format lacks read as a GPU samples them: 0 for a missing color, 255 for a missing alpha.
/// BC4 and BC5 follow babel::bcn, signed values included — their bias is documented there.
/// A legacy luminance format replicates into r, g and b.
[[nodiscard]] cc::result<cc::vector<byte>> decode_rgba8(data const& tex, subresource const& sub, decode_options opts = {});

/// Decode one subresource of `tex` to float r, g, b, a texels, row-major and top-left origin, slice after slice.
/// Every format is accepted: a normalized one lands in 0..1, or -1..1 where it is signed.
/// BC1 - BC5 come back as their exact interpolants, not the 8-bit decode's rounded ones.
/// sRGB formats come back still encoded, as their bytes are; converting to linear is the caller's.
[[nodiscard]] cc::result<cc::vector<float>> decode_rgba32f(data const& tex,
                                                           subresource const& sub,
                                                           decode_options opts = {});

/// decode_rgba32f into memory the caller owns, which must hold four floats per texel of `sub`.
/// For a caller that keeps the texels in a buffer of its own, so the decode is not copied a second time.
[[nodiscard]] cc::result<cc::unit> decode_rgba32f_into(data const& tex,
                                                       subresource const& sub,
                                                       cc::span<float> out,
                                                       decode_options opts = {});
} // namespace babel::dds
