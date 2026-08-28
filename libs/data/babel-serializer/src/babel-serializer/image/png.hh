#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream
#include <clean-core/string/string.hh>

namespace babel::png
{
struct write_options;
} // namespace babel::png

// Low-level PNG codec (image/).
//
// The faithful, format-shaped view of a PNG: decoded pixels PLUS the PNG's own metadata.
// That is the color type, bit depth, interlace, gamma, ICC profile, text chunks, physical dimensions, and more.
// This is the format layer — babel::image sits on top and throws the metadata away for the "just give me pixels" case.
//
// WHAT IS POPULATED TODAY.
// Everything below, read by the libspng backend: the IHDR fields, the pixels, and the ancillary chunks.
// Pixels are expanded / de-palettized / de-interlaced.
//
// `channels` follows the file rather than a fixed output shape: 1 grey, 2 grey+alpha, 3 rgb, 4 rgba,
// and one more than that wherever a tRNS chunk becomes an alpha channel.
//
// `decoded` is the sample type, and follows the file too: `u16` for a 16-bit PNG, `u8` for every other depth.
// Sub-byte depths (1/2/4) are unpacked to `u8`, so `bit_depth` — the file's own depth, verbatim — is the only
// place those survive.
// **16-bit samples are HOST-endian**, while the format stores them big-endian; the codec swaps in both
// directions, so a u16 view of `pixels` is directly readable and encode takes the same.
//
//   auto const img = babel::png::read(bytes).value();
//   auto const stride = img.width * img.channels * (img.decoded == babel::png::component::u16 ? 2 : 1);

/// Native PNG color type (IHDR byte 25). `palette` is de-palettized to rgb/rgba by the decoder — see `channels`.
enum class babel::png::color_type : babel::u8
{
    grey,       // 0
    rgb,        // 2
    palette,    // 3
    grey_alpha, // 4
    rgba,       // 6
};

/// Native PNG interlace method (IHDR byte 28). Adam7 is decoded transparently.
enum class babel::png::interlace_method : babel::u8
{
    none,  // 0
    adam7, // 1
};

/// Sample type of the decoded `pixels`: `u16` for a 16-bit PNG, `u8` for every other depth.
/// A `u16` sample is host-endian, not the format's big-endian.
enum class babel::png::component : babel::u8
{
    u8,
    u16,
};

/// One text chunk (tEXt / zTXt / iTXt).
/// On encode, an entry carrying a language or a translated keyword is written as iTXt, and `compressed` picks zTXt over tEXt.
struct babel::png::text_entry
{
    cc::string keyword;
    cc::string text;
    cc::string language;           // iTXt only
    cc::string translated_keyword; // iTXt only
    bool compressed = false;       // zTXt / compressed iTXt
};

/// Physical pixel dimensions (pHYs chunk).
struct babel::png::physical_dimensions
{
    int ppu_x = 0; // pixels per unit, x axis
    int ppu_y = 0; // pixels per unit, y axis
    bool unit_is_meter = false;
};

/// A faithful native decode of a PNG.
/// Read-once; deliberately not built for mutation.
struct babel::png::data
{
    int width = 0;
    int height = 0;
    int channels = 0;                    // samples per pixel in `pixels` (1 grey / 2 GA / 3 rgb / 4 rgba)
    int bit_depth = 8;                   // IHDR bit depth byte, verbatim: 1/2/4/8/16 in a valid file, unvalidated
    color_type color = color_type::rgba; // native IHDR color type (parsed natively)
    interlace_method interlace = interlace_method::none; // native IHDR interlace (parsed natively)
    component decoded = component::u8;                   // sample type of `pixels`, host-endian
    cc::vector<byte> pixels;                             // row-major, top-left origin, tightly packed

    cc::optional<double> gamma;                 // gAMA
    cc::optional<int> srgb_intent;              // sRGB rendering intent (0..3)
    cc::vector<byte> icc_profile;               // iCCP profile (inflated)
    cc::string icc_profile_name;                // iCCP profile name
    cc::vector<text_entry> texts;               // tEXt / zTXt / iTXt
    cc::optional<physical_dimensions> physical; // pHYs
    // ... cHRM, bKGD, tRNS, sBIT, tIME — add fields as the walker lands ...

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }
};

namespace babel::png
{

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole PNG buffer.
/// Errors on a bad signature / IHDR or a decode failure.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::png

/// PNG encode knobs.
/// PNG is lossless, so nothing here trades quality — only encode time against file size.
struct babel::png::write_options
{
    /// zlib's Deflate level: 0 (store) to 9 (smallest), or -1 for zlib's own default.
    int compression_level = -1;
};

namespace babel::png
{

/// Encode `img`'s pixels and metadata to PNG file bytes.
/// The written depth comes from `decoded` (8 or 16) and the color type from `channels`, so a decode / encode
/// round-trip preserves the samples at their own width, plus the chunks above them.
/// `bit_depth` and `interlace` are ignored — they describe a file that was read, not one being written, so a
/// sub-byte-depth PNG re-encodes as 8-bit and an Adam7 one re-encodes non-interlaced.
[[nodiscard]] cc::result<cc::vector<byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::png
