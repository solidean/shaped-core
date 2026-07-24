#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream
#include <clean-core/string/string.hh>

// Low-level JPEG codec (image/).
//
// The format-shaped view of a JPEG: decoded pixels PLUS the JPEG's own metadata (precision, progressive vs baseline, chroma subsampling, JFIF density, ICC profile, EXIF, comments).
// babel::image sits on top and keeps only the pixels.
//
// WHAT IS POPULATED TODAY.
// Pixels come from the stb backend (8-bit samples).
// The structural SOF/JFIF fields (bit_depth / progressive / chroma / jfif_density) are parsed natively by walking the marker segments up to the first scan.
// The variable-length metadata below the pixels in `data` is DESIGNED but [todo]: reassembling the ICC profile across APP2 markers, the APP1 EXIF block and the COM comments needs more of the marker walker.
// The fields exist now so that lands without an API change.
//
//   auto const img = babel::jpg::read(bytes).value();
//   auto const stride = img.width * img.channels; // tightly packed, top-left origin

namespace babel::jpg
{
/// Chroma subsampling of the luma plane (derived from the SOF sampling factors).
enum class subsampling : u8
{
    s444, // 4:4:4 — full chroma
    s422, // 4:2:2 — horizontal halving
    s420, // 4:2:0 — horizontal + vertical halving
    unknown,
};

/// JFIF pixel density unit (APP0 density_units byte).
enum class density_unit : u8
{
    none, // 0 — x/y are an aspect ratio only
    dpi,  // 1 — dots per inch
    dpcm, // 2 — dots per centimeter
};

/// JFIF pixel density (APP0). [populated when a JFIF APP0 marker is present]
struct density
{
    density_unit unit = density_unit::none;
    int x = 0;
    int y = 0;
};

/// A faithful native decode of a JPEG. Read-once.
struct data
{
    // --- populated now ---
    int width = 0;
    int height = 0;
    int channels = 0;                          // samples per pixel in `pixels` (1 grey / 3 rgb)
    int bit_depth = 8;                         // SOF sample precision (native)
    bool progressive = false;                  // SOF2 vs baseline SOF0 (native)
    subsampling chroma = subsampling::unknown; // native, from SOF sampling factors
    cc::optional<density> jfif_density;        // native, APP0 JFIF
    cc::vector<cc::byte> pixels;               // row-major, top-left origin, tightly packed

    // --- designed now, [todo] populate via more of the native marker walker (stb exposes none of these) ---
    cc::vector<cc::byte> icc_profile; // APP2 ICC_PROFILE, reassembled across markers in order
    cc::vector<cc::byte> exif;        // APP1 Exif block, verbatim
    cc::vector<cc::string> comments;  // COM markers

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }
};

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole JPEG buffer. Errors on a bad SOI marker or a decode failure.
[[nodiscard]] cc::result<data> read(cc::span<cc::byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

/// JPEG encode knobs. JPEG is lossy.
struct write_options
{
    int quality = 90; // 1..100
};

/// Encode `img`'s pixels to JPEG file bytes. Metadata fields stb cannot emit are ignored (see the header note).
[[nodiscard]] cc::result<cc::vector<cc::byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::jpg
