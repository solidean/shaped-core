#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream

namespace babel::pfm
{
struct write_options;
} // namespace babel::pfm

// Low-level PFM (Portable FloatMap) codec (image/).
//
// The format-shaped view of a `.pfm`: raw f32 samples plus the three things its header says about them —
// colour vs greyscale, the byte order, and the scale factor.
// babel::image sits on top and keeps only the pixels.
//
// This codec is NATIVE — the stb backend is not involved, and nothing here is [todo].
// PFM is the whole format: a five-token ASCII header, then width * height * channels IEEE-754 floats.
//
// PFM STORES BITS, WHERE HDR STORES A SHARED EXPONENT.
// A round-trip through PFM is exact, which is what makes it the format to reach for when the values matter more
// than the file size — a depth buffer, a normal map, an intermediate render target.
//
// TWO NORMALIZATIONS HAPPEN ON READ, so the decoded buffer matches every other babel image.
// PFM's rows run bottom-to-top and its samples carry the byte order the header declared;
// `pixels` come back top-left origin, in host byte order.
// `byte_order` still reports what the file used.
//
//   auto const img = babel::pfm::read(bytes).value();
//   auto const values = img.samples_f32(); // width * height * channels, top-left origin

/// Byte order of the samples as they sit in the file — the SIGN of the header's scale line.
enum class babel::pfm::endianness : babel::u8
{
    little, // negative scale
    big,    // positive scale
};

/// A faithful native decode of a PFM file.
/// Read-once.
struct babel::pfm::data
{
    int width = 0;
    int height = 0;
    int channels = 0; // 3 for the "PF" magic, 1 for "Pf"

    /// The magnitude of the header's scale line — the factor samples are meant to be multiplied by.
    /// The decoder does NOT apply it: it is metadata, and applying it would make a round-trip lossy.
    float scale = 1.0f;

    endianness byte_order = endianness::little; // what the file used; `pixels` are always host order

    cc::vector<byte> pixels; // f32 samples, row-major, top-left origin, tightly packed, host byte order

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }

    /// `pixels` viewed as f32 samples — width * height * channels of them.
    [[nodiscard]] cc::span<float const> samples_f32() const;
};

namespace babel::pfm
{

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole PFM buffer.
/// Errors on a magic other than `PF` / `Pf`, an unreadable header token, a zero scale, or a sample buffer that
/// falls short of what the header describes.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

} // namespace babel::pfm

/// PFM encode knobs.
/// The scale factor is not one of them — it travels in `data::scale`, so a decoded file re-encodes to the same header.
struct babel::pfm::write_options
{
    /// Byte order to write the samples in, which is also the sign of the scale line.
    /// Little-endian is what every current platform reads fastest and what most writers emit.
    endianness byte_order = endianness::little;
};

namespace babel::pfm
{

/// Encode `img`'s f32 samples to PFM file bytes.
/// `pixels` must hold width * height * channels floats, and `channels` must be 1 or 3 — PFM has no other shape.
/// Rows go out bottom-to-top, as the format requires; `img.pixels` stay top-left origin.
[[nodiscard]] cc::result<cc::vector<byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::pfm
