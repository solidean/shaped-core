#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream

namespace babel::webp
{
struct write_options;
} // namespace babel::webp

// Low-level WebP codec (image/).
//
// The format-shaped view of a still `.webp`: 8-bit RGB or RGBA pixels, plus whether the file was lossy or lossless.
// babel::image sits on top and keeps only the pixels.
// Pixels come from the vendored libwebp, both ways.
//
// A STILL IMAGE ONLY.
// An animated WebP is read as its first frame, and the containers' metadata (ICC, EXIF, XMP) is neither read nor written.
//
// LOSSY WEBP IS 4:2:0 YUV, LOSSLESS IS EXACT ARGB.
// A lossy encode converts to YUV with chroma at half resolution, as JPEG mostly does, and alpha is coded on its own.
// `write_options::lossless` round-trips every sample, alpha included, except the color under alpha 0, which `exact` keeps too.
//
//   auto const img = babel::webp::read(bytes).value();
//   auto const bytes2 = babel::webp::encode(img, {.quality = 75}).value();

/// A decoded WebP.
/// Read-once.
struct babel::webp::data
{
    int width = 0;
    int height = 0;

    /// 3 for RGB, 4 for RGBA: a decode is RGBA exactly when the file has alpha.
    /// An encode also takes 1 (grey) and 2 (grey + alpha), stored as RGB and RGBA.
    int channels = 0;

    /// Whether the file is lossless (VP8L); a decode reports it, an encode takes `write_options::lossless` instead.
    bool is_lossless = false;

    cc::vector<byte> pixels; // u8 samples, row-major, top-left origin, tightly packed

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }
};

namespace babel::webp
{

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole WebP buffer.
/// Errors on bytes libwebp does not take for a WebP, or a bitstream it cannot decode.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::webp

/// WebP encode knobs.
struct babel::webp::write_options
{
    /// 0 to 100: for a lossy encode, the quality; for a lossless one, how hard it works for a smaller file.
    int quality = 75;

    /// Lossless (VP8L) instead of lossy (VP8 plus a separate alpha plane).
    bool lossless = false;

    /// 0 (fastest) to 6 (slowest, smallest), libwebp's own method knob; 4 is its default.
    int method = 4;

    /// Keep the RGB under fully transparent pixels.
    /// libwebp's default replaces it with whatever codes cheapest, which a texture sampled bilinearly would bleed in.
    bool exact = false;
};

namespace babel::webp
{

/// Encode `img`'s u8 samples to WebP file bytes.
/// `channels` must be 1 to 4 and `pixels` must hold width * height * channels bytes; WebP stores at most 16383 per side.
[[nodiscard]] cc::result<cc::vector<byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::webp
