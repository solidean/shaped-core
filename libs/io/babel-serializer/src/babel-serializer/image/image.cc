#include <babel-serializer/image/image.hh>

// The aggregator delegates to the low-level codecs and never includes the stb backend directly.
#include <babel-serializer/image/jpg.hh>
#include <babel-serializer/image/png.hh>

namespace babel::image
{
namespace
{
/// Map a low-level decode's channel count + (future) sample type onto the aggregator's component enum.
/// Both codecs produce 8-bit samples today, so this is u8 for now — the switch is where u16 / f32 slot in.
component component_of_decode()
{
    return component::u8;
}
} // namespace

int image::bytes_per_component() const
{
    switch (comp)
    {
    case component::u8:
        return 1;
    case component::u16:
        return 2;
    case component::f32:
        return 4;
    }
    return 1;
}

isize image::row_stride() const
{
    return isize(width) * isize(channels) * isize(bytes_per_component());
}

cc::result<format> detect_format(cc::span<cc::byte const> bytes)
{
    // PNG opens with the 8-byte signature; the first four bytes are enough to disambiguate.
    if (bytes.size() >= 4 && bytes[0] == cc::byte(0x89) && bytes[1] == cc::byte(0x50) && bytes[2] == cc::byte(0x4E)
        && bytes[3] == cc::byte(0x47))
        return format::png;

    // JPEG opens with the SOI marker FF D8.
    if (bytes.size() >= 2 && bytes[0] == cc::byte(0xFF) && bytes[1] == cc::byte(0xD8))
        return format::jpg;

    return cc::error("image: unrecognized format (magic bytes match neither PNG nor JPEG)");
}

cc::result<image> read(cc::span<cc::byte const> bytes)
{
    auto fmt = detect_format(bytes);
    CC_RETURN_IF_ERROR(fmt);

    switch (fmt.value())
    {
    case format::png:
    {
        auto decoded = babel::png::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component_of_decode()};
        result.pixels = cc::move(d.pixels);
        return cc::move(result);
    }
    case format::jpg:
    {
        auto decoded = babel::jpg::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component_of_decode()};
        result.pixels = cc::move(d.pixels);
        return cc::move(result);
    }
    }

    return cc::error("image: unhandled format");
}

cc::result<image> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<cc::byte>> encode(image const& img, format fmt, write_options opts)
{
    if (img.is_empty())
        return cc::error("image encode: empty image");

    switch (fmt)
    {
    case format::png:
    {
        auto pd = babel::png::data{.width = img.width, .height = img.height, .channels = img.channels};
        pd.pixels = img.pixels; // aggregator owns only the packed buffer; hand it to the codec
        return babel::png::encode(pd);
    }
    case format::jpg:
    {
        auto jd = babel::jpg::data{.width = img.width, .height = img.height, .channels = img.channels};
        jd.pixels = img.pixels;
        return babel::jpg::encode(jd, {.quality = opts.jpg_quality});
    }
    }

    return cc::error("image encode: unhandled format");
}

cc::result<cc::unit> write(cc::write_stream& out, image const& img, format fmt, write_options opts)
{
    auto encoded = encode(img, fmt, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::image
