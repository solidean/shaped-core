#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>

// Low-level KTX 2.0 reader (image/).
//
// The format-shaped view of a `.ktx2`: the header and level index as stored, and every subresource — each mip level,
// array layer and cube face — as a view of its bytes.
// The texels are what a GPU uploads: block-compressed formats stay blocks, and nothing is transcoded.
//
// SUPERCOMPRESSION IS UNDONE ON READ, LEVEL BY LEVEL.
// A level stored plain is a zero-copy view of the input; a Zstandard or zlib level is decoded into a buffer of its own.
// The levels decode independently, so `read_options::scheduler` spreads them over a pool, and `read_async` runs a whole
// read on one.
// Basis Universal (BasisLZ, and UASTC's `VK_FORMAT_UNDEFINED`) is refused: it needs a transcoder, which babel does not carry.
//
// A malformed file is an error, never a partial read.
// The header must be self-consistent, every level must lie inside the input, and every level must hold exactly the bytes
// its extent and format imply, after supercompression.
//
// THE FORMAT IS VULKAN'S, BY NUMBER.
// `format`'s values are the `vkFormat`s, so a Vulkan host can cast it; a format babel does not name is an error.
//
//   auto const tex = babel::ktx2::read(cc::make_pinned_data(cc::move(file_bytes))).value();
//   auto const& top = *tex.find(0, 0, 0);    // layer 0, face 0, the base level
//   upload(tex.format, top.width, top.height, top.bytes.span());

/// What the texels of a KTX2 file are: a `vkFormat`, for the ones babel names.
enum class babel::ktx2::format : babel::u32
{
    r8_unorm = 9,
    r8_snorm = 10,
    r8_srgb = 15,
    r8g8_unorm = 16,
    r8g8_snorm = 17,
    r8g8_srgb = 22,
    r8g8b8a8_unorm = 37,
    r8g8b8a8_snorm = 38,
    r8g8b8a8_srgb = 43,
    b8g8r8a8_unorm = 44,
    b8g8r8a8_srgb = 50,
    a2b10g10r10_unorm_pack32 = 64,
    r16_unorm = 70,
    r16_sfloat = 76,
    r16g16_unorm = 77,
    r16g16_sfloat = 83,
    r16g16b16a16_unorm = 91,
    r16g16b16a16_sfloat = 97,
    r32_sfloat = 100,
    r32g32_sfloat = 103,
    r32g32b32a32_sfloat = 109,
    b10g11r11_ufloat_pack32 = 122,
    e5b9g9r9_ufloat_pack32 = 123,
    bc1_rgb_unorm = 131,
    bc1_rgb_srgb = 132,
    bc1_rgba_unorm = 133,
    bc1_rgba_srgb = 134,
    bc2_unorm = 135,
    bc2_srgb = 136,
    bc3_unorm = 137,
    bc3_srgb = 138,
    bc4_unorm = 139,
    bc4_snorm = 140,
    bc5_unorm = 141,
    bc5_snorm = 142,
    bc6h_ufloat = 143,
    bc6h_sfloat = 144,
    bc7_unorm = 145,
    bc7_srgb = 146,
};

/// How each level's bytes are stored.
enum class babel::ktx2::supercompression : babel::u32
{
    none = 0,
    basis_lz = 1, // refused: it needs a transcoder
    zstd = 2,
    zlib = 3,
};

/// Where one level is in the file: `byte_length` stored bytes, `uncompressed_byte_length` once supercompression is undone.
struct babel::ktx2::level_index
{
    u64 byte_offset = 0;
    u64 byte_length = 0;
    u64 uncompressed_byte_length = 0;
};

/// The header and index as the file stores them.
/// Counts keep the file's zeros: a `layer_count` of 0 is a texture that is not an array, a `level_count` of 0 one that
/// asks its loader to generate the mips below the one level it stores.
struct babel::ktx2::header
{
    u32 vk_format = 0;
    u32 type_size = 0;
    u32 pixel_width = 0;
    u32 pixel_height = 0;
    u32 pixel_depth = 0;
    u32 layer_count = 0;
    u32 face_count = 0;
    u32 level_count = 0;
    supercompression scheme = supercompression::none;

    u32 dfd_byte_offset = 0;
    u32 dfd_byte_length = 0;
    u32 kvd_byte_offset = 0;
    u32 kvd_byte_length = 0;
    u64 sgd_byte_offset = 0;
    u64 sgd_byte_length = 0;

    /// Level 0 is the base level, whatever order the file stores the levels in.
    cc::vector<level_index> levels;
};

/// One entry of the key/value data: `KTXorientation`, `KTXwriter`, or anything a writer added.
struct babel::ktx2::key_value
{
    cc::string key;
    /// The value as stored, a view sharing the input's owner; a text value keeps its terminating NUL.
    cc::pinned_data<byte const> value;
};

/// One mip, layer and face of the texture, and its texels.
struct babel::ktx2::subresource
{
    int mip = 0;
    int layer = 0; // the array element; 0 for a texture that is not an array
    int face = 0;  // the cube face, 0..5 for +X -X +Y -Y +Z -Z; 0 for a texture that is not a cube

    int width = 0;
    int height = 0;
    int depth = 1; // slices of a volume level, stored one after another

    /// The texels: blocks for a compressed format, tightly packed rows otherwise.
    /// A view of the input for a plain level, of its decoded buffer for a supercompressed one; either way it owns what it views.
    cc::pinned_data<byte const> bytes;
};

/// A faithful read of a KTX2 file.
/// Read-once.
struct babel::ktx2::data
{
    header file_header;

    format pixel_format = format::r8g8b8a8_unorm;

    int width = 0;
    int height = 1; // 1 for a 1D texture, whose header stores 0
    int depth = 1;  // 1 for a texture that is not 3D, whose header stores 0
    int mip_count = 1;
    int layer_count = 1; // array elements; a cube array counts cubes, not faces
    int face_count = 1;  // 6 for a cube map

    /// The data format descriptor's transfer function is sRGB.
    /// The format says so too for every sRGB one babel names; this is the descriptor's own word.
    bool is_srgb_transfer = false;
    /// The data format descriptor flags the color channels as premultiplied by alpha.
    bool is_premultiplied_alpha = false;

    cc::vector<key_value> key_values;

    /// Every subresource, level by level from the base, each level's layers in order, each layer's faces in order.
    cc::vector<subresource> subresources;

    [[nodiscard]] bool is_cube() const { return face_count == 6; }
    [[nodiscard]] bool is_array() const { return file_header.layer_count > 0; }

    /// The subresource at that position, or nullptr where the texture has none.
    [[nodiscard]] subresource const* find(int layer, int face, int mip) const;

    /// The value of key `key`, or nullptr where the file has none.
    [[nodiscard]] key_value const* find_key(cc::string_view key) const;
};

/// Read knobs.
struct babel::ktx2::read_options
{
    /// The most bytes the decoded levels of a supercompressed file may take together; more is an error before anything is allocated.
    /// A plain file's levels are views of its input, so this bounds only what a few compressed bytes can demand.
    /// The default holds a 16384 x 16384 BC7 texture with its mips; the largest `i64` disables the check.
    i64 max_decoded_bytes = i64(1) << 30;

    /// Where the levels of a supercompressed file decode: each level is one task.
    /// Null decodes them on the calling thread, one after another; the result is the same either way.
    /// The calling thread takes part, and the scheduler must outlive the read.
    cc::async_scheduler* scheduler = nullptr;
};

namespace babel::ktx2
{
/// The extent of mip `level` along an axis whose base is `base`, never below 1.
/// `level` must be in 0..31, since it is a shift of an `int`.
[[nodiscard]] constexpr int mip_extent(int base, int level)
{
    auto const e = base >> level;
    return e < 1 ? 1 : e;
}

/// True for the BC1 - BC7 formats, which store 4x4 blocks rather than texels.
[[nodiscard]] bool is_block_compressed(format fmt);

/// True for the formats whose samples are floats: BC6H and the 16- and 32-bit float ones, and the packed float ones.
[[nodiscard]] bool is_float(format fmt);

/// True for the sRGB-encoded formats.
[[nodiscard]] bool is_srgb(format fmt);

/// The bytes of one texel, or of one 4x4 block for a block-compressed format.
[[nodiscard]] int block_byte_size(format fmt);

/// The bytes one `width` x `height` x `depth` image of `fmt` occupies, rows of blocks for a compressed format.
[[nodiscard]] i64 image_byte_size(format fmt, int width, int height, int depth);

/// True when `fmt`'s value is a `vkFormat` babel names.
[[nodiscard]] bool is_known(u32 vk_format);

/// The format's name, for diagnostics.
[[nodiscard]] cc::string_view to_string(format fmt);

/// True when `bytes` begins with the KTX 2.0 identifier: a KTX2 file, valid or not.
[[nodiscard]] bool has_identifier(cc::span<byte const> bytes);

// reading
// -------------------------------------------------------------------------------------------------

/// Read a whole KTX2 file.
/// The zero-copy entry point: a plain level's subresources are views of `bytes` sharing its owner.
/// Errors on a missing identifier, a header that contradicts itself, a format babel does not name, Basis Universal,
/// a level outside the input or of the wrong size, and supercompressed data that does not decode.
[[nodiscard]] cc::result<data> read(cc::pinned_data<byte const> const& bytes, read_options opts = {});

/// Convenience: slurps the stream, then pins the slurped buffer (moved into the pin, not copied).
[[nodiscard]] cc::result<data> read(cc::read_stream& in, read_options opts = {});

/// Convenience: COPIES the input into an owned pin first, so the subresources stay valid independently of the caller's memory.
/// Pass a cc::pinned_data to avoid that copy.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes, read_options opts = {});

/// `read` as a task on `opts.scheduler`, or on `cc::compute_scheduler()` when that is null, scheduled before this returns.
/// Its levels decode on the same scheduler, so many textures read this way share one pool.
/// A file `read` refuses fails the task with the error `read` returns.
[[nodiscard]] cc::shared_async<data> read_async(cc::pinned_data<byte const> bytes, read_options opts = {});
} // namespace babel::ktx2
