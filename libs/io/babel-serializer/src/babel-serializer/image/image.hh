#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream

// Image aggregator (image/) — the "I just want pixel data" layer.
//
// Every image format decodes to the same shape (a packed pixel buffer), so this is babel's opinionated aggregator: one `image` struct, format-detecting read, explicit-format write.
// It sits ON TOP of the low-level per-format codecs (babel::png / babel::jpg) and delegates to them — it never touches the stb backend directly.
// Reach for a low-level codec instead when you need a format's metadata (color profile, gamma, EXIF, ...).
//
//   auto const img = babel::image::read(bytes).value();
//   auto const bytes2 = babel::image::encode(img, babel::image::format::png).value();

namespace babel::image
{
/// The image container formats babel can read and write.
enum class format : u8
{
    png,
    jpg,
};

/// Decoded sample type. Only `u8` is produced today; `u16` / `f32` are API-ready (16-bit PNG, HDR).
enum class component : u8
{
    u8,
    u16,
    f32,
};

/// Decoded pixels, row-major, top-left origin, tightly packed (row_stride == width * channels * bytes_per_component).
struct image
{
    int width = 0;
    int height = 0;
    int channels = 0; // 1 grey / 2 grey+alpha / 3 rgb / 4 rgba
    component comp = component::u8;
    cc::vector<cc::byte> pixels;

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }

    /// Bytes per single sample: 1 (u8) / 2 (u16) / 4 (f32).
    [[nodiscard]] int bytes_per_component() const;

    /// Bytes per pixel row: width * channels * bytes_per_component().
    [[nodiscard]] isize row_stride() const;
};

// reading
// -------------------------------------------------------------------------------------------------

/// Sniff the container format from the leading magic bytes. Errors if it matches no supported format.
[[nodiscard]] cc::result<format> detect_format(cc::span<cc::byte const> bytes);

/// Decode any supported image, auto-detecting the format and delegating to the matching low-level codec.
[[nodiscard]] cc::result<image> read(cc::span<cc::byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<image> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

/// Aggregator write knobs. `jpg_quality` is ignored for PNG.
struct write_options
{
    int jpg_quality = 90; // 1..100
};

/// Encode `img` to `fmt`'s file bytes, delegating to the matching low-level codec.
[[nodiscard]] cc::result<cc::vector<cc::byte>> encode(image const& img, format fmt, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, image const& img, format fmt, write_options opts = {});
} // namespace babel::image
