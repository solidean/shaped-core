#include "fixtures/png_fixtures.hh"

#include <babel-serializer/image/hdr.hh>
#include <babel-serializer/image/image.hh>
#include <babel-serializer/image/jpg.hh>
#include <babel-serializer/image/pfm.hh>
#include <babel-serializer/image/png.hh>
#include <clean-core/bytes/compression.hh>
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

/// A 16-bit gradient as a packed byte buffer of HOST-endian u16 samples, which is what the codec reads and writes.
///
/// A round-trip through the codec cannot tell host order from big-endian, since a missing swap on both sides
/// cancels out; what it does pin is that the two directions agree, which is what a caller depends on.
/// That the order is the HOST's rests on libspng's u16_row_to_host / u16_row_to_bigendian, both applied for
/// every format except SPNG_FMT_RAW, which babel never asks for.
cc::vector<byte> make_gradient16(int width, int height, int channels)
{
    auto samples = cc::vector<u16>();
    samples.resize_to_uninitialized(i64(width) * height * channels);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const idx = (i64(y) * width + x) * channels + c;
                // Spread across the full 16-bit range, so a value that survived only its high byte would show.
                samples[idx] = u16((x * 4099 + y * 277 + c * 21031) & 0xFFFF);
            }

    auto out = cc::vector<byte>();
    out.resize_to_uninitialized(samples.size() * 2);
    cc::memcpy(out.data(), samples.data(), size_t(out.size()));
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

TEST("png - 16-bit samples round-trip at every channel count")
{
    for (auto const channels : {1, 2, 3, 4})
    {
        auto src = babel::png::data{.width = 5, .height = 3, .channels = channels, .decoded = babel::png::component::u16};
        src.pixels = make_gradient16(5, 3, channels);

        auto const encoded = babel::png::encode(src);
        REQUIRE(encoded.has_value());

        auto const d = babel::png::read(encoded.value());
        REQUIRE(d.has_value());
        CHECK(d.value().channels == channels);
        CHECK(d.value().bit_depth == 16); // written from `decoded`, read back from IHDR
        CHECK(d.value().decoded == babel::png::component::u16);
        CHECK(pixels_equal(d.value().pixels, src.pixels)); // lossless at 16 bits too
        CHECK(d.value().pixels.size() == i64(5) * 3 * channels * 2);
    }
}

TEST("png - a 16-bit sample keeps its low byte")
{
    // The failure this pins is a silent narrow to 8 bits, which would leave every sample a multiple of 257.
    auto src = babel::png::data{.width = 2, .height = 1, .channels = 1, .decoded = babel::png::component::u16};
    src.pixels.resize_to_uninitialized(4);
    auto const written = cc::vector<u16>{u16(0x1234), u16(0xABCD)};
    cc::memcpy(src.pixels.data(), written.data(), 4);

    auto const d = babel::png::read(babel::png::encode(src).value());
    REQUIRE(d.has_value());
    REQUIRE(d.value().pixels.size() == 4);

    auto read_back = cc::vector<u16>();
    read_back.resize_to_uninitialized(2);
    cc::memcpy(read_back.data(), d.value().pixels.data(), 4);
    CHECK(read_back[0] == 0x1234);
    CHECK(read_back[1] == 0xABCD);
}

TEST("png - an 8-bit decode still reports u8")
{
    // The 16-bit path must not have widened the ordinary one.
    auto const d = babel::png::read(img::encode(make_gradient(3, 3, 3), img::format::png).value());
    REQUIRE(d.has_value());
    CHECK(d.value().decoded == babel::png::component::u8);
    CHECK(d.value().bit_depth == 8);
}

TEST("image - the aggregator carries the sample type both ways")
{
    auto src = img::image{.width = 4, .height = 2, .channels = 3, .comp = img::component::u16};
    src.pixels = make_gradient16(4, 2, 3);
    CHECK(src.bytes_per_component() == 2);
    CHECK(src.row_stride() == 4 * 3 * 2);

    auto const encoded = img::encode(src, img::format::png);
    REQUIRE(encoded.has_value());

    auto const d = img::read(encoded.value());
    REQUIRE(d.has_value());
    CHECK(d.value().comp == img::component::u16); // the aggregator used to hardcode u8 here
    CHECK(pixels_equal(d.value().pixels, src.pixels));

    // Neither format takes f32, and JPEG does not take u16 either — both say so rather than narrowing.
    auto wrong = src;
    wrong.comp = img::component::f32;
    CHECK(img::encode(wrong, img::format::png).has_error());
    CHECK(img::encode(src, img::format::jpg).has_error());
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

TEST("png - encode rejects a text body it could only write truncated")
{
    // libspng's encoder measures the body with strlen, so an embedded NUL would silently drop everything after it.
    // An empty keyword and an empty body are already errors; this is the same class of caller mistake, and the only
    // one of the three that would lose data rather than fail.
    auto img = babel::png::data{.width = 1, .height = 1, .channels = 1, .pixels = make_gradient(1, 1, 1).pixels};
    img.texts.push_back({.keyword = "Comment", .text = cc::string_view("before\0after", 12)});
    CHECK(babel::png::encode(img).has_error());
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

// Decode-only fixtures
// -------------------------------------------------------------------------------------------------
// babel::png::encode always writes a non-interlaced, non-palette, tRNS-free file at 8 or 16 bits, so a
// round-trip reaches exactly one of plan_decode's five branches.
// Four of the PNGs hand-built by fixtures/make-png-fixtures.py reach the other four; the fifth reaches
// no branch at all, being a header the decode must refuse before it sizes anything from it.

namespace
{
cc::span<byte const> fixture(cc::span<unsigned char const> bytes)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(bytes.data()), bytes.size());
}

/// PNG's own CRC-32, over chunk type + body.
/// A chunk carrying a wrong one is silently discarded by libspng rather than reported, so a hand-built
/// chunk that is meant to be REJECTED has to be otherwise valid or the test proves nothing.
u32 png_crc32(cc::span<byte const> data)
{
    auto crc = u32(0xFFFFFFFF);
    for (auto const b : data)
    {
        crc ^= u32(u8(b));
        for (auto bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & u32(-i32(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFF;
}

void push_u32_be(cc::vector<byte>& out, u32 v)
{
    out.push_back(byte(v >> 24));
    out.push_back(byte(v >> 16));
    out.push_back(byte(v >> 8));
    out.push_back(byte(v));
}

void push_chunk(cc::vector<byte>& out, cc::string_view type, cc::span<byte const> body)
{
    auto typed = cc::vector<byte>();
    for (auto const c : type)
        typed.push_back(byte(c));
    typed.push_back_range(body);

    push_u32_be(out, u32(body.size()));
    out.push_back_range(typed);
    push_u32_be(out, png_crc32(typed));
}

/// A zTXt chunk whose body inflates to `inflated_bytes`, well past max_png_chunk_bytes.
/// Deliberately built here rather than committed: 20 MB of zeros deflates to ~20 KB, which is ~122 KB of hex
/// in the generated fixture header for one behaviour the decode only has to refuse.
cc::vector<byte> ztxt_bomb(i64 inflated_bytes)
{
    auto payload = cc::vector<byte>();
    payload.resize_to_constructed(inflated_bytes, byte(0));

    auto body = cc::vector<byte>();
    for (auto const c : cc::string_view("Comment"))
        body.push_back(byte(c));
    body.push_back(byte(0)); // keyword terminator
    body.push_back(byte(0)); // compression method 0 = deflate
    // A zTXt body's compressed half is a zlib stream, which is exactly this framing.
    body.push_back_range(cc::compress(
        payload, {.algorithm = cc::compression_algorithm::deflate, .framing = cc::compression_framing::zlib}));
    return body;
}

/// A 1x1 grey PNG carrying one zTXt bomb, placed before or after IDAT.
/// Which side it sits on decides which libspng call trips over it, and both must report the ceiling.
cc::vector<byte> png_with_ztxt_bomb(bool after_idat)
{
    auto ihdr = cc::vector<byte>();
    push_u32_be(ihdr, 1);    // width
    push_u32_be(ihdr, 1);    // height
    ihdr.push_back(byte(8)); // bit depth
    ihdr.push_back(byte(0)); // color type: greyscale
    ihdr.push_back(byte(0)); // compression
    ihdr.push_back(byte(0)); // filter
    ihdr.push_back(byte(0)); // interlace

    auto scanline = cc::vector<byte>();
    scanline.push_back(byte(0)); // filter type None
    scanline.push_back(byte(0)); // the single sample
    auto const idat = cc::compress(
        scanline, {.algorithm = cc::compression_algorithm::deflate, .framing = cc::compression_framing::zlib});

    auto const bomb = ztxt_bomb(20 * 1024 * 1024);

    auto out = cc::vector<byte>();
    for (auto const c : cc::string_view("\x89PNG\r\n\x1a\n"))
        out.push_back(byte(c));
    push_chunk(out, "IHDR", ihdr);
    if (!after_idat)
        push_chunk(out, "zTXt", bomb);
    push_chunk(out, "IDAT", idat);
    if (after_idat)
        push_chunk(out, "zTXt", bomb);
    push_chunk(out, "IEND", {});
    return out;
}
} // namespace

TEST("png - a hostile IHDR is refused before its pixels are allocated")
{
    // 65535x65535 RGBA16 is just under 32 GiB, and spng_decoded_image_size answers from the 13-byte IHDR alone.
    // The per-axis cap passes it, so only the decoded-size ceiling stands between this <100 byte file and that
    // allocation — which is why the error text is what this asserts on, not merely that an error came back.
    auto const d = babel::png::read(fixture(babel_test::png_hostile_ihdr));
    REQUIRE(d.has_error());
    CHECK(d.error().to_string().contains("ceiling"));
}

TEST("png - an oversized zTXt is refused on either side of IDAT")
{
    // Before IDAT the chunk walk trips over it; after IDAT only spng_decode_chunks reaches it.
    // The second is the one that regresses quietly: a discarded return code leaves the context invalid and the
    // next getter reports a state error naming neither the limit nor the chunk.
    auto const before = babel::png::read(png_with_ztxt_bomb(false));
    REQUIRE(before.has_error());

    auto const after = babel::png::read(png_with_ztxt_bomb(true));
    REQUIRE(after.has_error());
    CHECK(after.error().to_string().contains("decode_chunks"));
}

TEST("png - a palette file de-palettizes, and its tRNS becomes alpha")
{
    auto const d = babel::png::read(fixture(babel_test::png_indexed_trns));
    REQUIRE(d.has_value());

    auto const& p = d.value();
    CHECK(p.width == 4);
    CHECK(p.height == 2);
    CHECK(p.color == babel::png::color_type::palette); // the file's own type, reported verbatim
    CHECK(p.bit_depth == 8);
    CHECK(p.channels == 4); // de-palettized to rgb, plus the alpha tRNS adds
    CHECK(p.decoded == babel::png::component::u8);
    REQUIRE(p.pixels.size() == 4 * 2 * 4);

    // Row 0 is palette entries 0..3: black, red, green, blue, with entry 0 transparent.
    auto const px = [&](int x, int y, int c) { return int(u8(p.pixels[(i64(y) * 4 + x) * 4 + c])); };
    CHECK(px(0, 0, 3) == 0);   // entry 0 is the one tRNS zeroes
    CHECK(px(1, 0, 0) == 255); // red
    CHECK(px(1, 0, 3) == 255);
    CHECK(px(2, 0, 1) == 255); // green
    CHECK(px(3, 0, 2) == 255); // blue
}

TEST("png - a one-bit greyscale file unpacks to u8 samples")
{
    auto const d = babel::png::read(fixture(babel_test::png_grey_1bit));
    REQUIRE(d.has_value());

    auto const& p = d.value();
    CHECK(p.width == 8);
    CHECK(p.height == 2);
    CHECK(p.color == babel::png::color_type::grey);
    CHECK(p.bit_depth == 1); // the only place the sub-byte depth survives
    CHECK(p.channels == 1);
    CHECK(p.decoded == babel::png::component::u8);
    REQUIRE(p.pixels.size() == 8 * 2); // one byte per pixel, not one bit

    // Row 0 is 0b10101010, so the samples alternate.
    CHECK(int(u8(p.pixels[0])) == 255);
    CHECK(int(u8(p.pixels[1])) == 0);
}

TEST("png - a grey file's tRNS becomes an alpha channel")
{
    auto const d = babel::png::read(fixture(babel_test::png_grey_trns));
    REQUIRE(d.has_value());

    auto const& p = d.value();
    CHECK(p.width == 4);
    CHECK(p.height == 2);
    CHECK(p.color == babel::png::color_type::grey); // one channel in the file
    CHECK(p.bit_depth == 8);
    CHECK(p.channels == 2); // two out of it, because tRNS adds the alpha
    REQUIRE(p.pixels.size() == 4 * 2 * 2);

    // Row 0 greys are 0, 64, 128, 255 and tRNS names 128, so exactly that pixel is transparent.
    auto const alpha = [&](int x, int y) { return int(u8(p.pixels[(i64(y) * 4 + x) * 2 + 1])); };
    CHECK(alpha(0, 0) == 255);
    CHECK(alpha(1, 0) == 255);
    CHECK(alpha(2, 0) == 0);
    CHECK(alpha(3, 0) == 255);
}

TEST("png - an Adam7 file is de-interlaced")
{
    auto const d = babel::png::read(fixture(babel_test::png_rgb_adam7));
    REQUIRE(d.has_value());

    auto const& p = d.value();
    CHECK(p.width == 8);
    CHECK(p.height == 8);
    CHECK(p.interlace == babel::png::interlace_method::adam7); // the file's own method, reported verbatim
    CHECK(p.color == babel::png::color_type::rgb);
    CHECK(p.channels == 3);
    REQUIRE(p.pixels.size() == 8 * 8 * 3);

    // The fixture's pixel value is a function of (x, y) alone, so every pass landing in the right place
    // is checkable rather than merely plausible.
    auto ok = true;
    for (auto y = 0; y < 8; ++y)
        for (auto x = 0; x < 8; ++x)
        {
            auto const at = (i64(y) * 8 + x) * 3;
            ok = ok && u8(p.pixels[at + 0]) == u8(x * 32 % 256);
            ok = ok && u8(p.pixels[at + 1]) == u8(y * 32 % 256);
            ok = ok && u8(p.pixels[at + 2]) == u8((x + y) * 16 % 256);
        }
    CHECK(ok);
}
