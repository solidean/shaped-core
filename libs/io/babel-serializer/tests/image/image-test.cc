#include <babel-serializer/image/image.hh>
#include <babel-serializer/image/jpg.hh>
#include <babel-serializer/image/png.hh>
#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <nexus/test.hh>

// stb is committed and always linked, so there is no availability branch here (unlike sqlite's is_available()).
// The happy path must simply work.

namespace
{
namespace img = babel::image;

/// A packed pixel image with a deterministic gradient, so round-trips have real content to compare.
img::image make_gradient(int width, int height, int channels)
{
    auto out = img::image{.width = width, .height = height, .channels = channels, .comp = img::component::u8};
    out.pixels.resize_to_uninitialized(cc::i64(width) * height * channels);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const idx = (cc::i64(y) * width + x) * channels + c;
                out.pixels[idx] = cc::byte((x * 8 + y * 4 + c * 32) & 0xFF);
            }
    return out;
}

bool pixels_equal(cc::span<cc::byte const> a, cc::span<cc::byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (cc::i64 i = 0; i < a.size(); ++i)
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

    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>(encoded.value()));
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
    for (cc::i64 i = 0; i < d.pixels.size(); ++i)
    {
        auto const delta = int(cc::u8(d.pixels[i])) - int(cc::u8(src.pixels[i]));
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
    cc::byte const garbage[6]
        = {cc::byte('n'), cc::byte('o'), cc::byte('p'), cc::byte('e'), cc::byte('!'), cc::byte(0)};
    CHECK(img::read(cc::span<cc::byte const>(garbage)).has_error());
    CHECK(img::detect_format(cc::span<cc::byte const>(garbage)).has_error());

    // a PNG-signed buffer is not valid JPEG: the low-level jpg reader rejects it on the SOI marker
    auto const png_bytes = img::encode(make_gradient(2, 2, 4), img::format::png).value();
    CHECK(babel::jpg::read(png_bytes).has_error());

    // encoding an empty image fails
    auto const empty = img::image{};
    CHECK(img::encode(empty, img::format::png).has_error());
    CHECK(img::encode(empty, img::format::jpg).has_error());
}
