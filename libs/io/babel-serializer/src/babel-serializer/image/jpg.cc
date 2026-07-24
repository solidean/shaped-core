#include <babel-serializer/image/impl/stb_backend.hh>
#include <babel-serializer/image/jpg.hh>

namespace babel::jpg
{
namespace
{
/// The structural fields recovered from a native SOF/JFIF marker walk (best-effort; defaults if a marker is absent).
struct header_scan
{
    int bit_depth = 8;
    bool progressive = false;
    subsampling chroma = subsampling::unknown;
    cc::optional<density> jfif_density;
};

int read_be_u16(cc::span<cc::byte const> bytes, isize offset)
{
    return (int(cc::u8(bytes[offset])) << 8) | int(cc::u8(bytes[offset + 1]));
}

density_unit density_unit_from_byte(int v)
{
    switch (v)
    {
    case 1:
        return density_unit::dpi;
    case 2:
        return density_unit::dpcm;
    default:
        return density_unit::none;
    }
}

subsampling subsampling_from_luma(int h, int v)
{
    if (h == 1 && v == 1)
        return subsampling::s444;
    if (h == 2 && v == 1)
        return subsampling::s422;
    if (h == 2 && v == 2)
        return subsampling::s420;
    return subsampling::unknown;
}

/// Walk the JPEG marker segments up to the first scan (SOS), filling the structural fields we understand.
/// Purely best-effort and bounds-checked: a marker we do not model is skipped by its length, and anything malformed just stops the walk early — pixel geometry still comes from the decoder.
header_scan scan_headers(cc::span<cc::byte const> bytes)
{
    auto scan = header_scan{};
    auto const size = bytes.size();

    auto i = isize(2); // past the SOI (FF D8), validated by the caller
    while (i + 1 < size)
    {
        if (bytes[i] != cc::byte(0xFF))
        {
            ++i; // resync to the next marker prefix
            continue;
        }
        while (i < size && bytes[i] == cc::byte(0xFF)) // skip fill bytes
            ++i;
        if (i >= size)
            break;

        auto const marker = int(cc::u8(bytes[i]));
        ++i;

        // Standalone markers carry no length segment.
        auto const is_rst = marker >= 0xD0 && marker <= 0xD7;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || is_rst)
            continue;
        if (marker == 0xDA) // SOS: entropy-coded scan begins, header region is over
            break;

        if (i + 1 >= size)
            break;
        auto const seg_len = read_be_u16(bytes, i); // includes the 2 length bytes
        auto const seg_start = i + 2;
        if (seg_len < 2 || seg_start + (seg_len - 2) > size)
            break;
        auto const seg = bytes.subspan(seg_start).first_n(seg_len - 2);

        // SOF markers: C0..CF except DHT (C4), JPGn reserved (C8), DAC (CC).
        auto const is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof && seg.size() >= 6)
        {
            scan.progressive = marker == 0xC2;
            scan.bit_depth = int(cc::u8(seg[0]));
            auto const components = int(cc::u8(seg[5]));
            // Per-component: id(1), sampling(1: H<<4 | V), quant-table(1). Luma is component 0.
            if (components >= 1 && seg.size() >= 6 + 3)
            {
                auto const sampling = int(cc::u8(seg[6 + 1]));
                auto const h = (sampling >> 4) & 0x0F;
                auto const v = sampling & 0x0F;
                scan.chroma = components == 1 ? subsampling::s444 : subsampling_from_luma(h, v);
            }
        }
        else if (marker == 0xE0 && seg.size() >= 14) // APP0 JFIF: "JFIF\0" ver(2) units(1) xdens(2) ydens(2) ...
        {
            auto const is_jfif = seg[0] == cc::byte('J') && seg[1] == cc::byte('F') && seg[2] == cc::byte('I')
                              && seg[3] == cc::byte('F') && seg[4] == cc::byte(0x00);
            if (is_jfif)
            {
                scan.jfif_density = density{
                    .unit = density_unit_from_byte(int(cc::u8(seg[7]))),
                    .x = read_be_u16(seg, 8),
                    .y = read_be_u16(seg, 10),
                };
            }
        }

        i = seg_start + (seg_len - 2);
    }

    return scan;
}
} // namespace

cc::result<data> read(cc::span<cc::byte const> bytes)
{
    if (bytes.size() < 2 || bytes[0] != cc::byte(0xFF) || bytes[1] != cc::byte(0xD8))
        return cc::error("jpg: bad SOI marker (not a JPEG)");

    auto decoded = babel::impl::stb_decode(bytes, 0);
    CC_RETURN_IF_ERROR(decoded);
    auto& px = decoded.value();

    auto const scan = scan_headers(bytes);

    auto result = data{
        .width = px.width,
        .height = px.height,
        .channels = px.channels,
        .bit_depth = scan.bit_depth,
        .progressive = scan.progressive,
        .chroma = scan.chroma,
        .jfif_density = scan.jfif_density,
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

cc::result<cc::vector<cc::byte>> encode(data const& img, write_options opts)
{
    if (img.is_empty())
        return cc::error("jpg encode: empty image");
    return babel::impl::stb_encode_jpg(img.pixels, img.width, img.height, img.channels, opts.quality);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::jpg
