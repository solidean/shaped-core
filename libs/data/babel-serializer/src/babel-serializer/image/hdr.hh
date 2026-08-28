#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream
#include <clean-core/string/string.hh>

namespace babel::hdr
{
struct write_options;
} // namespace babel::hdr

// Low-level Radiance HDR codec (image/).
//
// The format-shaped view of a `.hdr` / `.pic`: decoded f32 radiance PLUS the Radiance header's own variables.
// babel::image sits on top and keeps only the pixels.
//
// This codec is NATIVE — the stb backend is not involved, and nothing here is [todo].
// Both scanline encodings are read (the adaptive per-component RLE, and flat RGBE with the old `1 1 1 n` repeat marker),
// and every `KEY=value` header line survives in `variables` verbatim.
//
// RGBE IS A SHARED EXPONENT, NOT A FLOAT TRIPLE.
// Three 8-bit mantissas share one 8-bit exponent, so the channels of a pixel are quantized against their own maximum:
// the relative error is ~0.2% of that maximum, and a channel far below it loses precision accordingly.
// It is a lossy encoding of the f32 values handed to `encode`, unlike PFM, which stores the bits.
//
// OUR ENCODER ROUNDS THE MANTISSA; THE COMMON WRITERS TRUNCATE IT.
// Rounding on the way out is what lets the decoder read a mantissa back unshifted, which keeps an exactly-zero
// channel exactly zero — the pairing is ours, and a round-trip through this codec is centered.
// stb_image_write and Radiance's own writer truncate instead, so a foreign `.hdr` decodes about half a mantissa
// step low, systematically rather than as noise.
// That is inside the ~0.2% budget above, and `read` takes no knob for it: nothing in a file says which writer
// produced it.
//
//   auto const img = babel::hdr::read(bytes).value();
//   auto const rgb = img.samples_f32(); // width * height * 3 linear radiance values, top-left origin

/// Radiance `FORMAT=` line: what the file says its four bytes per pixel mean.
/// Both are the same shared-exponent encoding, and differ only in what the three mantissas are channels OF.
enum class babel::hdr::pixel_format : babel::u8
{
    rgbe, // 32-bit_rle_rgbe — RGB radiance, the common case
    xyze, // 32-bit_rle_xyze — CIE XYZ radiance
};

/// One `KEY=value` line of the Radiance header, verbatim.
struct babel::hdr::header_variable
{
    cc::string key;
    cc::string value;
};

/// A faithful native decode of a Radiance HDR file.
/// Read-once.
struct babel::hdr::data
{
    int width = 0;
    int height = 0;
    int channels = 3;                         // RGBE always decodes to three samples per pixel
    pixel_format format = pixel_format::rgbe; // FORMAT= line

    // --- how the file stored what `pixels` now holds ---
    bool stored_bottom_up = false;   // the file used `+Y`; `pixels` are normalized to a top-left origin either way
    bool run_length_encoded = false; // scanlines used the adaptive per-component RLE rather than flat RGBE

    // --- header variables, best-effort: a field keeps its default when the file names no such line ---
    cc::optional<double> exposure;         // EXPOSURE=, the PRODUCT of every such line, as Radiance defines it
    cc::optional<double> pixel_aspect;     // PIXASPECT=
    cc::string software;                   // SOFTWARE=
    cc::vector<header_variable> variables; // every KEY=value line, in file order, including the three above

    cc::vector<byte> pixels; // f32 samples, row-major, top-left origin, tightly packed

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }

    /// `pixels` viewed as f32 samples — width * height * channels of them.
    [[nodiscard]] cc::span<float const> samples_f32() const;
};

namespace babel::hdr
{

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole Radiance HDR buffer.
/// Errors on a missing `#?` magic, an unreadable resolution line, a FORMAT this codec cannot interpret,
/// or scanline data that runs out early.
/// Only the two row-major resolution lines are supported (`-Y h +X w` and `+Y h +X w`); a column-major file is an error.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::hdr

/// Radiance HDR encode knobs.
struct babel::hdr::write_options
{
    /// Emit the adaptive per-component RLE, which every reader since 1991 understands.
    /// Off writes flat RGBE — bigger, and the path an ancient reader takes.
    /// A row narrower than 8 or wider than 0x7fff pixels is flat regardless: the RLE marker is indistinguishable
    /// from a pixel at those widths, which is why the format itself excludes them.
    bool run_length_encode = true;
};

namespace babel::hdr
{

/// Encode `img`'s f32 samples to Radiance HDR file bytes.
/// `pixels` must hold width * height * 3 floats; `channels` must be 3.
/// The written header carries FORMAT plus EXPOSURE / PIXASPECT / SOFTWARE where those are set — `variables` is ignored,
/// and rows always go out top-down (`-Y`), so `stored_bottom_up` is ignored too.
[[nodiscard]] cc::result<cc::vector<byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::hdr
