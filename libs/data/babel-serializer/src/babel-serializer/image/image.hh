#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh>

namespace babel::image
{
struct write_options;
} // namespace babel::image
// cc::read_stream / cc::write_stream

// Image aggregator (image/) — the "I just want pixel data" layer.
//
// Every image format decodes to the same shape, a packed pixel buffer, so this is babel's opinionated aggregator:
// one `image` struct, format-detecting read, explicit-format write.
// It sits ON TOP of the low-level per-format codecs (babel::png / babel::jpg / babel::hdr / babel::pfm) and delegates to them.
// Reach for a low-level codec instead when you need a format's metadata (color profile, gamma, EXIF, the PFM scale, ...).
//
// THE SAMPLE TYPE FOLLOWS THE FORMAT, and a caller that mixes the two gets an error rather than reinterpreted bytes.
// PNG and JPEG are `u8`; HDR and PFM are `f32`.
// So `comp` is worth reading after any `read` that did not pick the format itself, and `encode` rejects an image
// whose `comp` the target format cannot store.
//
//   auto const img = babel::image::read(bytes).value();
//   auto const bytes2 = babel::image::encode(img, babel::image::format::png).value();

/// The image container formats babel can read and write.
enum class babel::image::format : babel::u8
{
    png,
    jpg,
    hdr, // Radiance RGBE — f32 samples, 3 channels
    pfm, // Portable FloatMap — f32 samples, 1 or 3 channels
};

/// Decoded sample type.
/// `u8` for PNG / JPEG, `u16` for a 16-bit PNG (host-endian), `f32` for HDR / PFM.
/// PNG is the one format that spans two: which one a decode produced is what `comp` says.
enum class babel::image::component : babel::u8
{
    u8,
    u16,
    f32,
};

/// Decoded pixels, row-major, top-left origin, tightly packed (row_stride == width * channels * bytes_per_component).
struct babel::image::image
{
    int width = 0;
    int height = 0;
    int channels = 0; // 1 grey / 2 grey+alpha / 3 rgb / 4 rgba
    component comp = component::u8;
    cc::vector<byte> pixels;

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }

    /// Bytes per single sample: 1 (u8) / 2 (u16) / 4 (f32).
    [[nodiscard]] int bytes_per_component() const;

    /// Bytes per pixel row: width * channels * bytes_per_component().
    [[nodiscard]] isize row_stride() const;

    /// `pixels` viewed as f32 samples — empty unless `comp` is `f32`.
    [[nodiscard]] cc::span<float const> samples_f32() const;
};

namespace babel::image
{

// reading
// -------------------------------------------------------------------------------------------------

/// Sniff the container format from the leading magic bytes.
/// Errors if it matches no supported format.
/// Each of the four has a distinct signature, so this never guesses: `\x89PNG`, `FF D8`, `#?`, `PF` / `Pf`.
[[nodiscard]] cc::result<format> detect_format(cc::span<byte const> bytes);

/// Decode any supported image, auto-detecting the format and delegating to the matching low-level codec.
[[nodiscard]] cc::result<image> read(cc::span<byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<image> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::image

/// Aggregator write knobs.
/// Only the one knob a caller is likely to want per format; a low-level codec's `write_options` carries the rest.
struct babel::image::write_options
{
    int jpg_quality = 90; // 1..100, and ignored by every other format
};

namespace babel::image
{

/// Encode `img` to `fmt`'s file bytes, delegating to the matching low-level codec.
/// `img.comp` must be what the format stores — `u8` for PNG / JPEG, `f32` for HDR / PFM — and the channel count
/// must be one the format has (3 for HDR; 1 or 3 for PFM).
[[nodiscard]] cc::result<cc::vector<byte>> encode(image const& img, format fmt, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, image const& img, format fmt, write_options opts = {});
} // namespace babel::image
