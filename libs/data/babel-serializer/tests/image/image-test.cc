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

// Both image backends are committed and always linked, so there is no availability branch here (unlike sqlite's is_available()).
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

TEST("png - channel count follows the file's color type")
{
    // The decoder reports what the PNG itself holds rather than a fixed output shape,
    // and the libspng formats behind grey and grey+alpha are not the same one — see plan_decode.
    for (auto const channels : {1, 2, 3, 4})
    {
        auto const src = make_gradient(4, 3, channels);
        auto const encoded = img::encode(src, img::format::png);
        REQUIRE(encoded.has_value());

        auto const d = babel::png::read(encoded.value());
        REQUIRE(d.has_value());
        CHECK(d.value().channels == channels);
        CHECK(d.value().bit_depth == 8);
        CHECK(pixels_equal(d.value().pixels, src.pixels)); // lossless at every channel count
    }
}

TEST("png - grey and grey-alpha carry their own color type")
{
    auto const grey = babel::png::read(img::encode(make_gradient(3, 3, 1), img::format::png).value());
    REQUIRE(grey.has_value());
    CHECK(grey.value().color == babel::png::color_type::grey);

    auto const grey_alpha = babel::png::read(img::encode(make_gradient(3, 3, 2), img::format::png).value());
    REQUIRE(grey_alpha.has_value());
    CHECK(grey_alpha.value().color == babel::png::color_type::grey_alpha);
}

TEST("png - metadata survives an encode / decode round-trip")
{
    auto src = babel::png::data{.width = 4, .height = 2, .channels = 3};
    src.pixels = make_gradient(4, 2, 3).pixels;
    src.gamma = 0.45455;
    src.srgb_intent = 1;
    src.icc_profile_name = "test profile";
    src.icc_profile = cc::vector<byte>{byte(1), byte(2), byte(3), byte(4)};
    src.physical = babel::png::physical_dimensions{.ppu_x = 2835, .ppu_y = 2835, .unit_is_meter = true};
    src.texts.push_back({.keyword = "Author", .text = "shaped-core"});
    src.texts.push_back({.keyword = "Comment", .text = "compressed body", .compressed = true});

    auto const encoded = babel::png::encode(src);
    REQUIRE(encoded.has_value());

    auto const d = babel::png::read(encoded.value());
    REQUIRE(d.has_value());
    auto const& out = d.value();

    REQUIRE(out.gamma.has_value());
    // gAMA travels as a fixed-point hundred-thousandth, so the round-trip is near-exact rather than exact.
    CHECK(out.gamma.value() > 0.45454);
    CHECK(out.gamma.value() < 0.45456);
    REQUIRE(out.srgb_intent.has_value());
    CHECK(out.srgb_intent.value() == 1);

    CHECK(out.icc_profile_name == "test profile");
    CHECK(pixels_equal(out.icc_profile, src.icc_profile));

    REQUIRE(out.physical.has_value());
    CHECK(out.physical.value().ppu_x == 2835);
    CHECK(out.physical.value().unit_is_meter);

    REQUIRE(out.texts.size() == 2);
    CHECK(out.texts[0].keyword == "Author");
    CHECK(out.texts[0].text == "shaped-core");
    CHECK(!out.texts[0].compressed);
    CHECK(out.texts[1].keyword == "Comment");
    CHECK(out.texts[1].text == "compressed body");
    CHECK(out.texts[1].compressed); // zTXt, and it must come back marked as such
}

TEST("png - a text chunk with a language round-trips as iTXt")
{
    auto src = babel::png::data{.width = 2, .height = 2, .channels = 1};
    src.pixels = make_gradient(2, 2, 1).pixels;
    src.texts.push_back({.keyword = "Title", .text = "ein Bild", .language = "de", .translated_keyword = "Titel"});

    auto const d = babel::png::read(babel::png::encode(src).value());
    REQUIRE(d.has_value());
    REQUIRE(d.value().texts.size() == 1);

    auto const& text = d.value().texts[0];
    CHECK(text.keyword == "Title");
    CHECK(text.text == "ein Bild");
    CHECK(text.language == "de");
    CHECK(text.translated_keyword == "Titel");
}

TEST("png - compression level trades size against time, losslessly")
{
    auto const src = make_gradient(64, 64, 3);
    auto pd = babel::png::data{.width = 64, .height = 64, .channels = 3};
    pd.pixels = src.pixels;

    auto const stored = babel::png::encode(pd, {.compression_level = 0});
    auto const smallest = babel::png::encode(pd, {.compression_level = 9});
    REQUIRE(stored.has_value());
    REQUIRE(smallest.has_value());
    CHECK(smallest.value().size() < stored.value().size());

    // Whatever the level, the pixels come back exactly.
    auto const d = babel::png::read(smallest.value());
    REQUIRE(d.has_value());
    CHECK(pixels_equal(d.value().pixels, src.pixels));

    CHECK(babel::png::encode(pd, {.compression_level = 10}).has_error());
}

TEST("png - malformed input is rejected rather than decoded")
{
    // A valid signature and IHDR, then truncated before any IDAT.
    auto encoded = babel::png::encode(
                       babel::png::data{.width = 2, .height = 2, .channels = 3, .pixels = make_gradient(2, 2, 3).pixels})
                       .value();
    encoded.resize_down_to(40);
    CHECK(babel::png::read(encoded).has_error());

    // Right signature, garbage everywhere after it.
    auto truncated = cc::vector<byte>();
    truncated.push_back_range(cc::span<byte const>(encoded).subspan({.offset = 0, .size = 8}));
    CHECK(babel::png::read(truncated).has_error());
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
