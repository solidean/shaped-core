#include <babel-serializer/image/image.hh>
#include <babel-serializer/image/webp.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Lossless WebP stores every sample, so its round-trip is EQUALITY; a lossy one is held to a tolerance and to fewer bytes.

namespace
{
namespace webp = babel::webp;

/// A color gradient with a little noise on it, so a lossless encode has something to store; `channels` of 3 or 4,
/// alpha a ramp of its own that never reaches 0.
/// A fully transparent pixel's color is libwebp's to replace, unless the encode asks for `exact`.
webp::data make_gradient(int width, int height, int channels)
{
    auto out = webp::data{.width = width, .height = height, .channels = channels};
    out.pixels.resize_to_uninitialized(isize(width) * height * channels);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto* const p = out.pixels.data() + (isize(y) * width + x) * channels;
            auto h = u32(y * width + x) * 0x9E3779B1u; // a mixing hash, so the noise has no pattern to code
            h = (h ^ (h >> 15)) * 0x85EBCA6Bu;
            h = (h ^ (h >> 13)) * 0xC2B2AE35u;
            auto const noise = int((h ^ (h >> 16)) & 15);
            p[0] = byte(u8(x * 240 / (width - 1) + noise));
            p[1] = byte(u8(y * 240 / (height - 1) + noise));
            p[2] = byte(u8(28 + ((x + y) * 7) % 200)); // a sawtooth, for an edge every so often
            if (channels == 4)
                p[3] = byte(u8(1 + (x + y) * 254 / (width + height - 2)));
        }
    return out;
}

bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

f64 mean_difference(cc::span<byte const> a, cc::span<byte const> b)
{
    auto sum = 0.0;
    for (auto i = isize(0); i < a.size(); ++i)
    {
        auto const d = int(u8(a[i])) - int(u8(b[i]));
        sum += d < 0 ? -d : d;
    }
    return sum / f64(a.size());
}
} // namespace

TEST("webp - lossless round-trips every sample, with or without alpha")
{
    for (auto const channels : {3, 4})
    {
        auto const img = make_gradient(37, 23, channels);
        auto const bytes = webp::encode(img, {.lossless = true});
        REQUIRE(bytes.has_value());
        auto const back = webp::read(bytes.value());
        REQUIRE(back.has_value());
        CHECK(back.value().width == 37);
        CHECK(back.value().height == 23);
        CHECK(back.value().channels == channels);
        CHECK(back.value().is_lossless);
        CHECK(same_bytes(back.value().pixels, img.pixels));
    }
}

TEST("webp - lossy stays close to the source in fewer bytes than lossless")
{
    auto const img = make_gradient(128, 96, 3);
    auto const lossy = webp::encode(img, {.quality = 80});
    auto const lossless = webp::encode(img, {.lossless = true});
    REQUIRE(lossy.has_value());
    REQUIRE(lossless.has_value());
    CHECK(lossy.value().size() < lossless.value().size());
    auto const back = webp::read(lossy.value());
    REQUIRE(back.has_value());
    CHECK(!back.value().is_lossless);
    CHECK(back.value().channels == 3);
    CHECK(mean_difference(back.value().pixels, img.pixels) < 8); // it drops the noise and blurs color edges (4:2:0)
}

TEST("webp - exact keeps the color under transparent pixels")
{
    // half the image fully transparent over a color it should keep
    auto img = make_gradient(32, 32, 4);
    for (auto i = isize(0); i < 32 * 16; ++i)
        img.pixels[i * 4 + 3] = byte(0);
    auto const bytes = webp::encode(img, {.lossless = true, .exact = true});
    REQUIRE(bytes.has_value());
    auto const back = webp::read(bytes.value());
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value().pixels, img.pixels));
}

TEST("webp - grey and grey with alpha are stored as RGB and RGBA")
{
    auto img = webp::data{.width = 8, .height = 8, .channels = 2};
    img.pixels.resize_to_uninitialized(8 * 8 * 2);
    for (auto i = 0; i < 64; ++i)
    {
        img.pixels[i * 2] = byte(u8(i * 4));
        img.pixels[i * 2 + 1] = byte(u8(255 - i));
    }
    auto const back = webp::read(webp::encode(img, {.lossless = true}).value());
    REQUIRE(back.has_value());
    REQUIRE(back.value().channels == 4);
    for (auto i = 0; i < 64; ++i)
    {
        CHECK(back.value().pixels[i * 4] == img.pixels[i * 2]);
        CHECK(back.value().pixels[i * 4 + 2] == img.pixels[i * 2]);
        CHECK(back.value().pixels[i * 4 + 3] == img.pixels[i * 2 + 1]);
    }
}

TEST("webp - errors: not a WebP, an empty image, too many channels, a side past 16383")
{
    auto const junk = cc::vector<byte>::create_filled(64, byte(7));
    CHECK(webp::read(junk).has_error());
    CHECK(webp::encode(webp::data{}).has_error());
    auto five = make_gradient(4, 4, 3);
    five.channels = 5;
    CHECK(webp::encode(five).has_error());
    auto wide = webp::data{.width = 16384, .height = 1, .channels = 3};
    wide.pixels.resize_to_uninitialized(16384 * 3);
    CHECK(webp::encode(wide).has_error());
}

TEST("webp - the image aggregator detects, reads and writes it")
{
    auto const src = make_gradient(20, 10, 4);
    auto const bytes = webp::encode(src, {.lossless = true}).value();
    auto const fmt = babel::image::detect_format(bytes);
    REQUIRE(fmt.has_value());
    CHECK(fmt.value() == babel::image::format::webp);

    auto const img = babel::image::read(bytes);
    REQUIRE(img.has_value());
    CHECK(img.value().comp == babel::image::component::u8);
    CHECK(img.value().channels == 4);
    CHECK(same_bytes(img.value().pixels, src.pixels));

    auto const again = babel::image::encode(img.value(), babel::image::format::webp, {.webp_lossless = true});
    REQUIRE(again.has_value());
    CHECK(same_bytes(babel::image::read(again.value()).value().pixels, src.pixels));

    // the sample type follows the format: a float image is refused rather than reinterpreted
    auto f = babel::image::image{.width = 2, .height = 2, .channels = 3, .comp = babel::image::component::f32};
    f.pixels.resize_to_uninitialized(2 * 2 * 3 * 4);
    CHECK(babel::image::encode(f, babel::image::format::webp).has_error());
}

TEST("webp - reads from a stream")
{
    auto const bytes = webp::encode(make_gradient(9, 9, 3), {.lossless = true}).value();
    auto adapter = cc::span_read_stream_adapter(cc::span<byte const>(bytes));
    cc::read_stream stream = adapter;
    auto const img = webp::read(stream);
    REQUIRE(img.has_value());
    CHECK(img.value().width == 9);
}
