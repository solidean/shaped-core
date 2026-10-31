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
// It sits ON TOP of the low-level per-format codecs (babel::png / babel::jpg / babel::hdr / babel::pfm / babel::dds /
// babel::tga) and delegates to them.
// Reach for a low-level codec instead when you need a format's metadata (color profile, gamma, EXIF, the PFM scale, ...).
//
// THE SAMPLE TYPE FOLLOWS THE FORMAT, and a caller that mixes the two gets an error rather than reinterpreted bytes.
// JPEG and TGA are `u8`, and HDR and PFM are `f32`.
// PNG spans two, `u16` for a 16-bit file and `u8` otherwise; DDS spans two as well, `f32` for BC6H and the float
// formats and `u8` for every other one.
// So `comp` is worth reading after any `read` that did not pick the format itself, and `encode` rejects an image
// whose `comp` the target format cannot store.
//
// A DDS FILE IS A WHOLE TEXTURE, AND THIS KEEPS ONE IMAGE OF IT.
// That is mip 0 of the first layer, the first face of a cube map, and the first slice of a volume.
// It always comes back as 4 channels, the r, g, b, a a GPU would sample, so BC5 is (x, y, 0, 1) and a normal's z is
// the consumer's to reconstruct.
// babel::dds has the rest of the texture, and the blocks themselves for an upload that skips decoding.
//
// DDS and TGA are read-only here: `encode` refuses both.
//
//   auto const img = babel::image::read(bytes).value();
//   auto const bytes2 = babel::image::encode(img, babel::image::format::png).value();

/// The image container formats babel reads; it writes every one but DDS and TGA.
enum class babel::image::format : babel::u8
{
    png,
    jpg,
    hdr, // Radiance RGBE — f32 samples, 3 channels
    pfm, // Portable FloatMap — f32 samples, 1 or 3 channels
    dds, // DirectDraw Surface — read-only; u8, or f32 for a float format, always 4 channels
    tga, // Truevision TARGA — read-only; u8 samples
};

/// Decoded sample type.
/// `u8` for PNG / JPEG / TGA, `u16` for a 16-bit PNG (host-endian), `f32` for HDR / PFM.
/// PNG and DDS each span two: which one a decode produced is what `comp` says.
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

/// Aggregator read knobs.
struct babel::image::read_options
{
    /// The most bytes a DDS or TGA decode may take, passed on as their own budget; past it the read is an error.
    /// PNG, HDR and PFM keep fixed ceilings of their own and take no budget, and neither does JPEG.
    /// The default holds an 8192 x 8192 decode to rgba8, but only 4096 x 4096 to rgba32f.
    /// A larger float texture, such as an 8192 x 4096 BC6H environment map, needs it raised.
    /// The largest `isize` disables the check.
    isize max_decoded_bytes = isize(1) << 28;
};

namespace babel::image
{

// reading
// -------------------------------------------------------------------------------------------------

/// Sniff the container format from its signature.
/// Errors if it matches no supported format.
/// Five formats open with a distinct magic, and those never guess: `\x89PNG`, `FF D8`, `#?`, `PF` / `Pf` and `DDS `.
/// TGA has none, so it is tried last: a TGA 2.0 footer identifies it, and failing that a header that reads as a
/// TGA's is taken for one.
/// That last test is a guess, so a caller that knows the format from elsewhere should pass it to `read`.
[[nodiscard]] cc::result<format> detect_format(cc::span<byte const> bytes);

/// Decode any supported image, auto-detecting the format and delegating to the matching low-level codec.
[[nodiscard]] cc::result<image> read(cc::span<byte const> bytes, read_options opts = {});

/// Decode `bytes` as `fmt`, skipping detection — for a caller that knows the format from a file extension or a MIME type.
[[nodiscard]] cc::result<image> read(cc::span<byte const> bytes, format fmt, read_options opts = {});

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<image> read(cc::read_stream& in, read_options opts = {});

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::image

/// Aggregator write knobs.
/// Only the one knob a caller is likely to want per format; a low-level codec's `write_options` carries the rest.
struct babel::image::write_options
{
    int jpg_quality = 90; // 1..100, and ignored by every other format

    /// zlib's Deflate level for a PNG, 0 (store) to 9 (smallest) or -1 for zlib's default, and ignored by every other format.
    /// 1080p RGB measured 304 ms at the default, 101 ms at 1 and 5 ms at 0 for a 5x larger file.
    int png_compression_level = -1;
};

namespace babel::image
{

/// Encode `img` to `fmt`'s file bytes, delegating to the matching low-level codec.
/// `img.comp` must be what the format stores — `u8` for JPEG, `u8` or `u16` for PNG, `f32` for HDR / PFM — and the
/// channel count must be one the format has (3 for HDR; 1 or 3 for PFM).
/// DDS and TGA cannot be encoded, and are an error.
/// A `u16` PNG encode writes 16-bit samples, so `comp` is what picks the written depth.
[[nodiscard]] cc::result<cc::vector<byte>> encode(image const& img, format fmt, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, image const& img, format fmt, write_options opts = {});
} // namespace babel::image
