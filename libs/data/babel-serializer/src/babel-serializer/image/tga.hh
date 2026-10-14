#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>

// Low-level TGA (Truevision TARGA) reader (image/).
//
// The format-shaped view of a `.tga`: 8-bit samples plus what its header says about them —
// the image type, the stored depth, the alpha bits and the corner the rows start from.
// babel::image sits on top and keeps only the pixels.
//
// Native, with no backend: an 18-byte header, an optional palette, then raw or run-length-encoded pixels.
// Color-mapped, true-color and grayscale images all read, raw and RLE alike.
//
// THREE NORMALIZATIONS HAPPEN ON READ, so the decoded buffer matches every other babel image.
// TGA stores blue before red, and `pixels` come back r, g, b(, a).
// It usually stores its rows bottom-to-top, and may store them right-to-left; `pixels` come back with a top-left origin.
// `stored_bottom_up` and `stored_right_to_left` still report what the file did.
//
// TGA HAS NO MAGIC NUMBER.
// A TGA 2.0 file ends in a "TRUEVISION-XFILE." footer, and an older one is recognized by its header alone.
// babel::image::detect_format tries every format with a signature first, for that reason.
//
//   auto const img = babel::tga::read(bytes).value();
//   auto const rgba = img.pixels; // width * height * channels bytes, top-left origin

/// What the pixels of a TGA file encode, from its image-type byte with the RLE bit taken out.
enum class babel::tga::image_type : babel::u8
{
    color_mapped, // types 1 and 9: indices into the palette
    true_color,   // types 2 and 10: 15/16, 24 or 32 bits a pixel
    grayscale,    // types 3 and 11: 8 bits a pixel, or 16 with alpha
};

/// A faithful native decode of a TGA file.
/// Read-once.
struct babel::tga::data
{
    int width = 0;
    int height = 0;

    /// 1 gray, 2 gray + alpha, 3 rgb or 4 rgba — what the stored pixels hold, a palette's entries for a color-mapped image.
    /// A 16-bit pixel has an alpha channel only when the header grants it the attribute bit.
    /// That alpha is bit 15, which many writers leave 0 whatever the header says, so such a file reads fully transparent.
    int channels = 0;

    image_type type = image_type::true_color;
    bool run_length_encoded = false;

    int pixel_depth = 0;     // bits per stored pixel: the index width for a color-mapped image
    int color_map_depth = 0; // bits per palette entry; 0 without a palette
    int alpha_bits = 0;      // the attribute bits the descriptor declares

    bool stored_bottom_up = true;      // the descriptor's vertical origin; `pixels` are top-left regardless
    bool stored_right_to_left = false; // the descriptor's horizontal origin

    int x_origin = 0;
    int y_origin = 0;

    cc::string image_id; // the header's free-form identification field, often empty

    cc::vector<byte> pixels; // u8 samples, row-major, top-left origin, tightly packed, r g b a order

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }
};

/// Read knobs.
struct babel::tga::read_options
{
    /// The most bytes the decoded pixels may take; a file describing more is an error before anything is allocated.
    /// A two-byte run-length packet describes up to 128 pixels, so 67 MB of packets could still describe 17 GB.
    /// The header and the data alone therefore never bound the allocation; this does.
    /// The default holds an 8192 x 8192 rgba image; the largest `isize` disables the check.
    isize max_decoded_bytes = isize(1) << 28;
};

namespace babel::tga
{
/// True when `bytes` end in the TGA 2.0 footer, the one signature the format has.
[[nodiscard]] bool has_footer(cc::span<byte const> bytes);

/// True when `bytes` open with a header a TGA writer would produce: a known image type, a matching color map and a
/// depth that type stores, with no obsolete interleave bits and no color-map fields on an image without a map.
/// Stricter than `read`, which accepts those last two, because a looser test takes more data of other kinds for a TGA.
/// A guess by construction, since TGA has no magic: data of another kind can still pass it.
[[nodiscard]] bool has_plausible_header(cc::span<byte const> bytes);

/// Decode a whole TGA buffer.
/// Errors on an image type it does not know, a depth that type cannot store, a palette index past the palette,
/// pixel data that falls short of what the header describes, or a decode larger than `opts` allows.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes, read_options opts = {});

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in, read_options opts = {});
} // namespace babel::tga
