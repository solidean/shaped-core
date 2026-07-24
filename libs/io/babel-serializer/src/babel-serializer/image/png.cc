#include <babel-serializer/image/impl/stb_backend.hh>
#include <babel-serializer/image/png.hh>
#include <clean-core/string/format.hh>

namespace babel::png
{
namespace
{
// The 8-byte PNG signature that opens every file.
constexpr cc::byte png_signature[8] = {
    cc::byte(0x89), cc::byte(0x50), cc::byte(0x4E), cc::byte(0x47), //
    cc::byte(0x0D), cc::byte(0x0A), cc::byte(0x1A), cc::byte(0x0A),
};

/// Read a big-endian u32 at `offset` (caller guarantees 4 readable bytes).
int read_be_u32(cc::span<cc::byte const> bytes, isize offset)
{
    auto const b0 = int(cc::u8(bytes[offset + 0]));
    auto const b1 = int(cc::u8(bytes[offset + 1]));
    auto const b2 = int(cc::u8(bytes[offset + 2]));
    auto const b3 = int(cc::u8(bytes[offset + 3]));
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

cc::result<color_type> color_type_from_byte(int v)
{
    switch (v)
    {
    case 0:
        return color_type::grey;
    case 2:
        return color_type::rgb;
    case 3:
        return color_type::palette;
    case 4:
        return color_type::grey_alpha;
    case 6:
        return color_type::rgba;
    default:
        return cc::error(cc::format("png: invalid IHDR color type {}", v));
    }
}

cc::result<interlace_method> interlace_from_byte(int v)
{
    switch (v)
    {
    case 0:
        return interlace_method::none;
    case 1:
        return interlace_method::adam7;
    default:
        return cc::error(cc::format("png: invalid IHDR interlace method {}", v));
    }
}
} // namespace

cc::result<data> read(cc::span<cc::byte const> bytes)
{
    // The IHDR chunk is fixed-layout and always first, so its structural fields need no full chunk walker:
    //   [0..7] signature, [8..11] length, [12..15] "IHDR", [16..19] width, [20..23] height,
    //   [24] bit depth, [25] color type, [26] compression, [27] filter, [28] interlace.
    if (bytes.size() < 29)
        return cc::error("png: buffer too small to hold a PNG header");

    for (auto i = 0; i < 8; ++i)
        if (bytes[i] != png_signature[i])
            return cc::error("png: bad signature (not a PNG)");

    if (bytes[12] != cc::byte('I') || bytes[13] != cc::byte('H') || bytes[14] != cc::byte('D')
        || bytes[15] != cc::byte('R'))
        return cc::error("png: missing IHDR chunk");

    auto color = color_type_from_byte(int(cc::u8(bytes[25])));
    CC_RETURN_IF_ERROR(color);
    auto interlace = interlace_from_byte(int(cc::u8(bytes[28])));
    CC_RETURN_IF_ERROR(interlace);

    // Pixels via the backend (8-bit, expanded / de-palettized / de-interlaced).
    auto decoded = babel::impl::stb_decode(bytes, 0);
    CC_RETURN_IF_ERROR(decoded);
    auto& px = decoded.value();

    auto result = data{
        .width = px.width,
        .height = px.height,
        .channels = px.channels,
        .bit_depth = int(cc::u8(bytes[24])), // native depth; decoded pixels stay 8-bit for now
        .color = color.value(),
        .interlace = interlace.value(),
        .decoded = component::u8,
    };
    result.pixels = cc::move(px.pixels);
    return cc::move(result);
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<cc::byte>> encode(data const& img, write_options)
{
    if (img.is_empty())
        return cc::error("png encode: empty image");
    return babel::impl::stb_encode_png(img.pixels, img.width, img.height, img.channels);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::png
