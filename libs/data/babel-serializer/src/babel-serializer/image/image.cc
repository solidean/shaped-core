#include <babel-serializer/image/image.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>

// The aggregator delegates to the low-level codecs and never includes the stb backend directly.
#include <babel-serializer/image/hdr.hh>
#include <babel-serializer/image/jpg.hh>
#include <babel-serializer/image/pfm.hh>
#include <babel-serializer/image/png.hh>

namespace babel::image
{
namespace
{
/// The sample types a container can hold.
/// This is the one place the mapping lives, so `read` and `encode` cannot disagree about it.
///
/// PNG is the one format that is not a single answer — 8- and 16-bit are both PNG — so its decode carries its own
/// in `babel::png::data::decoded`, and `read` asserts the result back against this.
bool format_accepts(format fmt, component comp)
{
    switch (fmt)
    {
    case format::png:
        return comp == component::u8 || comp == component::u16;
    case format::jpg:
        return comp == component::u8;
    case format::hdr:
    case format::pfm:
        return comp == component::f32;
    }
    return false;
}

/// The aggregator's spelling of a PNG decode's own sample type.
component component_of(babel::png::component c)
{
    return c == babel::png::component::u16 ? component::u16 : component::u8;
}

cc::string_view name_of(format fmt)
{
    switch (fmt)
    {
    case format::png:
        return "png";
    case format::jpg:
        return "jpg";
    case format::hdr:
        return "hdr";
    case format::pfm:
        return "pfm";
    }
    return "?";
}

cc::string_view name_of(component comp)
{
    switch (comp)
    {
    case component::u8:
        return "u8";
    case component::u16:
        return "u16";
    case component::f32:
        return "f32";
    }
    return "?";
}

/// What a format can store, for the error below — PNG is the one that names two.
cc::string_view accepted_of(format fmt)
{
    switch (fmt)
    {
    case format::png:
        return "u8 or u16";
    case format::jpg:
        return "u8";
    case format::hdr:
    case format::pfm:
        return "f32";
    }
    return "?";
}

cc::result<cc::unit> check_encode_component(image const& img, format fmt)
{
    if (!format_accepts(fmt, img.comp))
        return cc::error(cc::format("image encode: {} stores {} samples, but the image carries {}", //
                                    name_of(fmt), accepted_of(fmt), name_of(img.comp)));
    return cc::unit{};
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

cc::span<float const> image::samples_f32() const
{
    if (comp != component::f32)
        return {};

    auto const floats = cc::span<byte const>(pixels).try_reinterpret_as<float const>();
    return floats.has_value() ? floats.value() : cc::span<float const>();
}

cc::result<format> detect_format(cc::span<byte const> bytes)
{
    // PNG opens with the 8-byte signature; the first four bytes are enough to disambiguate.
    if (bytes.size() >= 4 && bytes[0] == byte(0x89) && bytes[1] == byte(0x50) && bytes[2] == byte(0x4E)
        && bytes[3] == byte(0x47))
        return format::png;

    // JPEG opens with the SOI marker FF D8.
    if (bytes.size() >= 2 && bytes[0] == byte(0xFF) && bytes[1] == byte(0xD8))
        return format::jpg;

    // Radiance opens with "#?", whatever identifier follows it (RADIANCE, RGBE, ...).
    if (bytes.size() >= 2 && bytes[0] == byte('#') && bytes[1] == byte('?'))
        return format::hdr;

    // PFM opens with "PF" (colour) or "Pf" (grey), and the format requires whitespace right after it.
    if (bytes.size() >= 3 && bytes[0] == byte('P') && (bytes[1] == byte('F') || bytes[1] == byte('f'))
        && (bytes[2] == byte('\n') || bytes[2] == byte('\r') || bytes[2] == byte(' ') || bytes[2] == byte('\t')))
        return format::pfm;

    return cc::error("image: unrecognized format (magic bytes match none of PNG, JPEG, Radiance HDR and PFM)");
}

cc::result<image> read(cc::span<byte const> bytes)
{
    // The aggregator's span rather than the codec's, so a trace shows "load an image" above whichever codec ran.
    CC_RECORD_SCOPE("image.read");
    CC_RECORD_ACCUM("image.bytes_decoded", cc::rec::unit_bytes, bytes.size());

    auto fmt = detect_format(bytes);
    CC_RETURN_IF_ERROR(fmt);

    switch (fmt.value())
    {
    case format::png:
    {
        auto decoded = babel::png::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result
            = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component_of(d.decoded)};
        CC_ASSERT(format_accepts(format::png, result.comp), "a PNG decode produced a sample type PNG cannot store");
        result.pixels = cc::move(d.pixels);
        return cc::move(result);
    }
    case format::jpg:
    {
        auto decoded = babel::jpg::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component::u8};
        result.pixels = cc::move(d.pixels);
        return cc::move(result);
    }
    case format::hdr:
    {
        auto decoded = babel::hdr::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component::f32};
        result.pixels = cc::move(d.pixels);
        return cc::move(result);
    }
    case format::pfm:
    {
        auto decoded = babel::pfm::read(bytes);
        CC_RETURN_IF_ERROR(decoded);
        auto& d = decoded.value();
        auto result = image{.width = d.width, .height = d.height, .channels = d.channels, .comp = component::f32};
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

cc::result<cc::vector<byte>> encode(image const& img, format fmt, write_options opts)
{
    CC_RECORD_SCOPE("image.encode");
    CC_RECORD_ACCUM("image.pixels_encoded", cc::rec::unit_count, f64(img.width) * f64(img.height));

    if (img.is_empty())
        return cc::error("image encode: empty image");

    CC_RETURN_IF_ERROR(check_encode_component(img, fmt));

    switch (fmt)
    {
    case format::png:
    {
        auto pd = babel::png::data{
            .width = img.width,
            .height = img.height,
            .channels = img.channels,
            // PNG is the format that accepts two sample types, so `comp` is what picks the written depth.
            .decoded = img.comp == component::u16 ? babel::png::component::u16 : babel::png::component::u8};
        pd.pixels = img.pixels; // aggregator owns only the packed buffer; hand it to the codec
        return babel::png::encode(pd);
    }
    case format::jpg:
    {
        auto jd = babel::jpg::data{.width = img.width, .height = img.height, .channels = img.channels};
        jd.pixels = img.pixels;
        return babel::jpg::encode(jd, {.quality = opts.jpg_quality});
    }
    case format::hdr:
    {
        auto hd = babel::hdr::data{.width = img.width, .height = img.height, .channels = img.channels};
        hd.pixels = img.pixels;
        return babel::hdr::encode(hd);
    }
    case format::pfm:
    {
        auto pd = babel::pfm::data{.width = img.width, .height = img.height, .channels = img.channels};
        pd.pixels = img.pixels;
        return babel::pfm::encode(pd);
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
