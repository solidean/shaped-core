#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream
#include <clean-core/string/string.hh>

// Low-level PNG codec (image/).
//
// The faithful, format-shaped view of a PNG: decoded pixels PLUS the PNG's own metadata (color type, bit depth, interlace, gamma, ICC profile, text chunks, physical dimensions, ...).
// This is the format layer — babel::image sits on top and throws the metadata away for the "just give me pixels" case.
//
// WHAT IS POPULATED TODAY.
// Pixels come from the stb backend (8-bit samples, expanded / de-palettized / de-interlaced), and the structural IHDR fields (bit_depth / color / interlace) are parsed natively.
// Everything below the pixels in `data` is DESIGNED but [todo]: stb exposes no metadata, so filling it needs a native chunk walker.
// The fields exist now so that walker lands without an API change.
// `bit_depth` is the file's native depth (1/2/4/8/16), but the decoded `pixels` are always 8-bit for now (`decoded == component::u8`); a 16-bit path is future work.
//
//   auto const img = babel::png::read(bytes).value();
//   auto const stride = img.width * img.channels; // tightly packed, top-left origin

namespace babel::png
{
/// Native PNG color type (IHDR byte 25). `palette` is de-palettized to rgb/rgba by the decoder — see `channels`.
enum class color_type : u8
{
    grey,       // 0
    rgb,        // 2
    palette,    // 3
    grey_alpha, // 4
    rgba,       // 6
};

/// Native PNG interlace method (IHDR byte 28). Adam7 is decoded transparently.
enum class interlace_method : u8
{
    none,  // 0
    adam7, // 1
};

/// Sample type of the decoded `pixels`. Only `u8` is produced today; `u16` (16-bit PNG) is API-ready.
enum class component : u8
{
    u8,
    u16,
};

/// One text chunk (tEXt / zTXt / iTXt). [todo] not populated yet — needs a native chunk walker.
struct text_entry
{
    cc::string keyword;
    cc::string text;
    cc::string language;           // iTXt only
    cc::string translated_keyword; // iTXt only
    bool compressed = false;       // zTXt / compressed iTXt
};

/// Physical pixel dimensions (pHYs chunk). [todo]
struct physical_dimensions
{
    int ppu_x = 0; // pixels per unit, x axis
    int ppu_y = 0; // pixels per unit, y axis
    bool unit_is_meter = false;
};

/// A faithful native decode of a PNG. Read-once; deliberately not built for mutation.
struct data
{
    // --- populated now ---
    int width = 0;
    int height = 0;
    int channels = 0;                    // samples per pixel in `pixels` (1 grey / 2 GA / 3 rgb / 4 rgba)
    int bit_depth = 8;                   // native IHDR bit depth: 1/2/4/8/16 (parsed natively)
    color_type color = color_type::rgba; // native IHDR color type (parsed natively)
    interlace_method interlace = interlace_method::none; // native IHDR interlace (parsed natively)
    component decoded = component::u8;                   // sample type of `pixels`
    cc::vector<cc::byte> pixels;                         // row-major, top-left origin, tightly packed

    // --- designed now, [todo] populate via a future native chunk walker (stb exposes none of these) ---
    cc::optional<double> gamma;                 // gAMA
    cc::optional<int> srgb_intent;              // sRGB rendering intent (0..3)
    cc::vector<cc::byte> icc_profile;           // iCCP profile (inflated)
    cc::string icc_profile_name;                // iCCP profile name
    cc::vector<text_entry> texts;               // tEXt / zTXt / iTXt
    cc::optional<physical_dimensions> physical; // pHYs
    // ... cHRM, bKGD, tRNS, sBIT, tIME — add fields as the walker lands ...

    [[nodiscard]] bool is_empty() const { return width <= 0 || height <= 0; }
};

// reading
// -------------------------------------------------------------------------------------------------

/// Decode a whole PNG buffer. Errors on a bad signature / IHDR or a decode failure.
[[nodiscard]] cc::result<data> read(cc::span<cc::byte const> bytes);

/// Convenience: slurp the stream to end, then decode.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

// writing
// -------------------------------------------------------------------------------------------------

/// PNG encode knobs. PNG is lossless; stb exposes no tuning today.
struct write_options
{
    // [todo] int compression_level once a non-stb encoder lands
};

/// Encode `img`'s pixels to PNG file bytes. Metadata fields stb cannot emit are ignored (see the header note).
[[nodiscard]] cc::result<cc::vector<cc::byte>> encode(data const& img, write_options opts = {});

/// Encode and write to a stream.
[[nodiscard]] cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts = {});
} // namespace babel::png
