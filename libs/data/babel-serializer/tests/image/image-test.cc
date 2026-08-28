#include <babel-serializer/image/hdr.hh>
#include <babel-serializer/image/image.hh>
#include <babel-serializer/image/jpg.hh>
#include <babel-serializer/image/pfm.hh>
#include <babel-serializer/image/png.hh>
#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// stb is committed and always linked, so there is no availability branch here (unlike sqlite's is_available()).
// The happy path must simply work.

namespace
{
namespace img = babel::image;

/// A packed pixel image with a deterministic gradient, so round-trips have real content to compare.
img::image make_gradient(int width, int height, int channels)
{
    auto out = img::image{.width = width, .height = height, .channels = channels, .comp = img::component::u8};
    out.pixels.resize_to_uninitialized(i64(width) * height * channels);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const idx = (i64(y) * width + x) * channels + c;
                out.pixels[idx] = byte((x * 8 + y * 4 + c * 32) & 0xFF);
            }
    return out;
}

/// The f32 counterpart, for the formats whose samples are floats.
img::image make_float_gradient(int width, int height, int channels)
{
    auto out = img::image{.width = width, .height = height, .channels = channels, .comp = img::component::f32};
    out.pixels.resize_to_uninitialized(i64(width) * height * channels * i64(sizeof(float)));

    auto* const samples = reinterpret_cast<float*>(out.pixels.data());
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < channels; ++c)
                samples[(i64(y) * width + x) * channels + c] = float(x + y * 2 + c) * 0.5f + 0.25f;

    return out;
}

bool pixels_equal(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (i64 i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

TEST("image - png round-trip is lossless")
{
    auto const src = make_gradient(5, 3, 4); // RGBA

    auto const encoded = img::encode(src, img::format::png);
    REQUIRE(encoded.has_value());

    auto const decoded = img::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.width == 5);
    CHECK(d.height == 3);
    CHECK(d.channels == 4);
    CHECK(d.comp == img::component::u8);
    CHECK(pixels_equal(d.pixels, src.pixels)); // PNG is lossless -> exact
}

TEST("png - low-level decode exposes native IHDR fields")
{
    auto const src = make_gradient(4, 4, 4);
    auto const encoded = img::encode(src, img::format::png);
    REQUIRE(encoded.has_value());

    auto const d = babel::png::read(encoded.value());
    REQUIRE(d.has_value());

    CHECK(d.value().width == 4);
    CHECK(d.value().height == 4);
    CHECK(d.value().channels == 4);
    CHECK(d.value().bit_depth == 8);
    CHECK(d.value().color == babel::png::color_type::rgba);
    CHECK(d.value().interlace == babel::png::interlace_method::none);
    CHECK(d.value().decoded == babel::png::component::u8);
}

TEST("image - png round-trips through the read_stream overload")
{
    auto const src = make_gradient(3, 2, 3); // RGB
    auto const encoded = img::encode(src, img::format::png);
    REQUIRE(encoded.has_value());

    auto adapter = cc::span_read_stream_adapter(cc::span<byte const>(encoded.value()));
    cc::read_stream stream = adapter;
    auto const decoded = img::read(stream);
    REQUIRE(decoded.has_value());

    CHECK(decoded.value().width == 3);
    CHECK(decoded.value().channels == 3);
    CHECK(pixels_equal(decoded.value().pixels, src.pixels));
}

TEST("image - jpg round-trip preserves geometry, pixels approximately")
{
    auto const src = make_gradient(16, 16, 3); // RGB; JPEG has no alpha

    auto const encoded = img::encode(src, img::format::jpg, {.jpg_quality = 95});
    REQUIRE(encoded.has_value());

    auto const decoded = img::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.width == 16);
    CHECK(d.height == 16);
    CHECK(d.channels == 3);
    REQUIRE(d.pixels.size() == src.pixels.size());

    // JPEG is lossy: assert closeness, not equality.
    auto max_delta = 0;
    for (i64 i = 0; i < d.pixels.size(); ++i)
    {
        auto const delta = int(u8(d.pixels[i])) - int(u8(src.pixels[i]));
        max_delta = cc::max(max_delta, delta < 0 ? -delta : delta);
    }
    CHECK(max_delta <= 40);
}

TEST("jpg - low-level decode exposes native SOF fields")
{
    auto const src = make_gradient(16, 16, 3);
    auto const encoded = img::encode(src, img::format::jpg, {.jpg_quality = 90});
    REQUIRE(encoded.has_value());

    auto const d = babel::jpg::read(encoded.value());
    REQUIRE(d.has_value());

    CHECK(d.value().width == 16);
    CHECK(d.value().height == 16);
    CHECK(d.value().channels == 3);
    CHECK(d.value().bit_depth == 8); // baseline 8-bit precision
    CHECK(!d.value().progressive);   // stb writes baseline JPEG
}

TEST("image - detect_format on png and jpg magic bytes")
{
    auto const png_src = make_gradient(2, 2, 4);
    auto const png_bytes = img::encode(png_src, img::format::png).value();
    auto const fmt_png = img::detect_format(png_bytes);
    REQUIRE(fmt_png.has_value());
    CHECK(fmt_png.value() == img::format::png);

    auto const jpg_src = make_gradient(8, 8, 3);
    auto const jpg_bytes = img::encode(jpg_src, img::format::jpg).value();
    auto const fmt_jpg = img::detect_format(jpg_bytes);
    REQUIRE(fmt_jpg.has_value());
    CHECK(fmt_jpg.value() == img::format::jpg);
}

TEST("image - error paths")
{
    // garbage bytes match no format
    byte const garbage[6] = {byte('n'), byte('o'), byte('p'), byte('e'), byte('!'), byte(0)};
    CHECK(img::read(cc::span<byte const>(garbage)).has_error());
    CHECK(img::detect_format(cc::span<byte const>(garbage)).has_error());

    // a PNG-signed buffer is not valid JPEG: the low-level jpg reader rejects it on the SOI marker
    auto const png_bytes = img::encode(make_gradient(2, 2, 4), img::format::png).value();
    CHECK(babel::jpg::read(png_bytes).has_error());

    // encoding an empty image fails
    auto const empty = img::image{};
    CHECK(img::encode(empty, img::format::png).has_error());
    CHECK(img::encode(empty, img::format::jpg).has_error());
}

TEST("image - hdr decodes to f32 through the aggregator")
{
    auto const src = make_float_gradient(16, 4, 3);

    auto const encoded = img::encode(src, img::format::hdr);
    REQUIRE(encoded.has_value());

    auto const decoded = img::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.width == 16);
    CHECK(d.height == 4);
    CHECK(d.channels == 3);
    CHECK(d.comp == img::component::f32);
    CHECK(d.bytes_per_component() == 4);
    CHECK(d.row_stride() == 16 * 3 * 4);

    // RGBE is lossy against a shared exponent, so this asserts closeness rather than the bytes.
    auto const a = src.samples_f32();
    auto const b = d.samples_f32();
    REQUIRE(a.size() == b.size());
    auto worst_relative = 0.0f;
    for (i64 i = 0; i < a.size(); ++i)
    {
        auto const delta = b[i] - a[i];
        worst_relative = cc::max(worst_relative, (delta < 0 ? -delta : delta) / a[i]);
    }
    CHECK(worst_relative < 0.01f);
}

TEST("image - pfm round-trips exactly through the aggregator")
{
    auto const src = make_float_gradient(5, 3, 3);

    auto const encoded = img::encode(src, img::format::pfm);
    REQUIRE(encoded.has_value());

    auto const decoded = img::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.comp == img::component::f32);
    CHECK(d.channels == 3);
    CHECK(pixels_equal(d.pixels, src.pixels)); // PFM stores bits -> exact
}

TEST("image - detect_format on hdr and pfm magic bytes")
{
    auto const hdr_bytes = img::encode(make_float_gradient(16, 2, 3), img::format::hdr).value();
    auto const fmt_hdr = img::detect_format(hdr_bytes);
    REQUIRE(fmt_hdr.has_value());
    CHECK(fmt_hdr.value() == img::format::hdr);

    auto const pfm_bytes = img::encode(make_float_gradient(4, 2, 3), img::format::pfm).value();
    auto const fmt_pfm = img::detect_format(pfm_bytes);
    REQUIRE(fmt_pfm.has_value());
    CHECK(fmt_pfm.value() == img::format::pfm);

    // greyscale PFM has its own magic, and must not be mistaken for anything else
    auto const grey_bytes = img::encode(make_float_gradient(4, 2, 1), img::format::pfm).value();
    REQUIRE(img::detect_format(grey_bytes).has_value());
    CHECK(img::detect_format(grey_bytes).value() == img::format::pfm);
}

TEST("image - encode rejects a sample type the format cannot store")
{
    auto const u8_image = make_gradient(8, 8, 3);
    CHECK(img::encode(u8_image, img::format::hdr).has_error());
    CHECK(img::encode(u8_image, img::format::pfm).has_error());

    auto const f32_image = make_float_gradient(8, 8, 3);
    CHECK(img::encode(f32_image, img::format::png).has_error());
    CHECK(img::encode(f32_image, img::format::jpg).has_error());

    // the channel count still has to be one the format has
    auto const f32_rgba = make_float_gradient(8, 8, 4);
    CHECK(img::encode(f32_rgba, img::format::hdr).has_error());
    CHECK(img::encode(f32_rgba, img::format::pfm).has_error());
}
