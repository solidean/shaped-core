#include <babel-serializer/image/hdr.hh>
#include <clean-core/common/utility.hh> // cc::max, cc::memcpy
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The Radiance codec is native — no backend to probe, and every path below is ours.
// What the tests pin: both scanline encodings in both directions, the two resolution lines, the header variables,
// and the accuracy RGBE's shared exponent actually delivers.

namespace
{
namespace hdr = babel::hdr;

/// An f32 RGB image whose channels within a pixel stay the same order of magnitude.
/// RGBE quantizes a pixel against its own maximum, so a triple spanning orders of magnitude is a test of the
/// format's limits rather than of this codec — `make_wide_range` below is that test, on purpose.
hdr::data make_gradient(int width, int height)
{
    auto out = hdr::data{.width = width, .height = height, .channels = 3};
    out.pixels.resize_to_uninitialized(isize(width) * height * 3 * isize(sizeof(float)));

    auto* const samples = reinterpret_cast<float*>(out.pixels.data());
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < 3; ++c)
                samples[(isize(y) * width + x) * 3 + c] = float(x * 3 + y * 7 + c) * 0.25f + 0.5f;

    return out;
}

/// Radiance values from 1e-3 to 1e+3, which is what an HDR file exists to carry.
hdr::data make_wide_range(int width, int height)
{
    auto out = hdr::data{.width = width, .height = height, .channels = 3};
    out.pixels.resize_to_uninitialized(isize(width) * height * 3 * isize(sizeof(float)));

    auto* const samples = reinterpret_cast<float*>(out.pixels.data());
    auto const count = isize(width) * height;
    for (auto i = isize(0); i < count; ++i)
    {
        auto const magnitude = 0.001f * float(1 << (i % 20)); // 1e-3 .. ~1e+3, a power of two apart
        samples[i * 3 + 0] = magnitude;
        samples[i * 3 + 1] = magnitude * 0.5f;
        samples[i * 3 + 2] = magnitude * 0.25f;
    }

    return out;
}

/// RGBE shares one exponent across a pixel, so the error budget is relative to the pixel's LARGEST channel.
bool round_trip_is_close(hdr::data const& src, hdr::data const& decoded)
{
    auto const a = src.samples_f32();
    auto const b = decoded.samples_f32();
    if (a.size() != b.size())
        return false;

    for (auto i = isize(0); i + 2 < a.size(); i += 3)
    {
        auto const largest = cc::max(cc::max(a[i], a[i + 1]), a[i + 2]);
        auto const tolerance = largest * 0.006f + 1e-9f;
        for (auto c = isize(0); c < 3; ++c)
        {
            auto const delta = b[i + c] - a[i + c];
            if ((delta < 0 ? -delta : delta) > tolerance)
                return false;
        }
    }
    return true;
}

/// A float built from its bits, which is how a test reaches infinity and NaN without <cmath> or <limits>.
float float_from_bits(u32 bits)
{
    auto value = 0.0f;
    cc::memcpy(&value, &bits, sizeof(value));
    return value;
}

void append_text(cc::vector<byte>& out, cc::string_view text)
{
    for (auto const c : text)
        out.push_back(byte(c));
}
} // namespace

TEST("hdr - rle round-trip keeps every pixel within the shared exponent's budget")
{
    auto const src = make_gradient(16, 8);

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.width == 16);
    CHECK(d.height == 8);
    CHECK(d.channels == 3);
    CHECK(d.format == hdr::pixel_format::rgbe);
    CHECK(d.run_length_encoded);
    CHECK(!d.stored_bottom_up);
    CHECK(round_trip_is_close(src, d));
}

TEST("hdr - flat scanlines round-trip too, and read back as flat")
{
    auto const src = make_gradient(16, 8);

    auto const encoded = hdr::encode(src, {.run_length_encode = false});
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(!decoded.value().run_length_encoded);
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - a scanline narrower than 8 pixels is flat whatever the options say")
{
    // The RLE marker is indistinguishable from a pixel at those widths, so the format excludes them.
    auto const src = make_gradient(4, 4);

    auto const encoded = hdr::encode(src, {.run_length_encode = true});
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(!decoded.value().run_length_encoded);
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - runs longer than one count byte survive the round-trip")
{
    // 300 identical pixels per row exceed the 127 a single run byte can spell, so the encoder must split them.
    auto src = hdr::data{.width = 300, .height = 3, .channels = 3};
    src.pixels.resize_to_uninitialized(isize(300) * 3 * 3 * isize(sizeof(float)));
    auto* const samples = reinterpret_cast<float*>(src.pixels.data());
    for (auto i = isize(0); i < isize(300) * 3 * 3; ++i)
        samples[i] = 2.0f;

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    // A constant image is where the RLE has to pay for itself: three rows of 300 pixels are 3600 flat bytes.
    CHECK(encoded.value().size() < 400);

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().run_length_encoded);
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - values spanning six orders of magnitude survive")
{
    auto const src = make_wide_range(20, 4);

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - black stays exactly black and a power of two stays exact")
{
    auto src = hdr::data{.width = 8, .height = 1, .channels = 3};
    src.pixels.resize_to_uninitialized(isize(8) * 3 * isize(sizeof(float)));
    auto* const samples = reinterpret_cast<float*>(src.pixels.data());
    for (auto i = isize(0); i < isize(8) * 3; ++i)
        samples[i] = 0.0f;

    // pixel 1 is a power of two in all three channels, which the mantissa can hold exactly
    samples[3] = 0.5f;
    samples[4] = 0.5f;
    samples[5] = 0.5f;

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());
    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const out = decoded.value().samples_f32();
    REQUIRE(out.size() == 24);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 0.0f);
    CHECK(out[2] == 0.0f);
    CHECK(out[3] == 0.5f);
    CHECK(out[4] == 0.5f);
    CHECK(out[5] == 0.5f);
}

TEST("hdr - infinity, NaN and huge finite values saturate rather than wrapping")
{
    // The encoder takes an exponent, which is only defined for a finite non-zero scalar — so what it does with
    // the values that have none is a contract, not an accident.
    auto src = hdr::data{.width = 8, .height = 1, .channels = 3};
    src.pixels.resize_to_uninitialized(isize(8) * 3 * isize(sizeof(float)));
    auto* const samples = reinterpret_cast<float*>(src.pixels.data());
    for (auto i = isize(0); i < isize(8) * 3; ++i)
        samples[i] = 1.0f;

    samples[0] = float_from_bits(0x7F800000);  // +infinity
    samples[3] = float_from_bits(0x7FC00000);  // NaN
    samples[6] = float_from_bits(0x7F7FFFFF);  // the largest finite float, past what the shared exponent holds
    samples[9] = -4.0f;                        // negative radiance has no RGBE spelling
    samples[12] = float_from_bits(0xFF800000); // -infinity

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());
    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const out = decoded.value().samples_f32();
    REQUIRE(out.size() == 24);
    CHECK(out[0] > 1e37f); // saturated to the largest RGBE value, not wrapped to something small
    CHECK(out[3] == 0.0f); // NaN has no magnitude, so the pixel is black
    CHECK(out[6] > 1e37f); // ... as is the largest finite float
    CHECK(out[9] == 0.0f); // negative and -infinity both clamp to black
    CHECK(out[12] == 0.0f);
    CHECK(out[15] == 1.0f); // and an ordinary pixel beside them is untouched
}

TEST("hdr - header variables are kept, and the parsed ones round-trip")
{
    auto src = make_gradient(8, 2);
    src.exposure = 2.5;
    src.pixel_aspect = 1.5;
    src.software = "babel-test";

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    REQUIRE(d.exposure.has_value());
    CHECK(d.exposure.value() == 2.5);
    REQUIRE(d.pixel_aspect.has_value());
    CHECK(d.pixel_aspect.value() == 1.5);
    CHECK(d.software == "babel-test");

    // FORMAT plus the three above, in file order, verbatim.
    REQUIRE(d.variables.size() == 4);
    CHECK(d.variables[0].key == "FORMAT");
    CHECK(d.variables[0].value == "32-bit_rle_rgbe");
    CHECK(d.variables[1].key == "EXPOSURE");
    CHECK(d.variables[3].key == "SOFTWARE");
    CHECK(d.variables[3].value == "babel-test");
}

TEST("hdr - the xyze format is read and written as itself")
{
    auto src = make_gradient(8, 2);
    src.format = hdr::pixel_format::xyze;

    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = hdr::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().format == hdr::pixel_format::xyze);
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - a +Y file is flipped into a top-left origin")
{
    // Hand-built, because our own encoder only ever writes -Y: two flat scanlines, the first of them the BOTTOM row.
    auto bytes = cc::vector<byte>();
    append_text(bytes, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+Y 2 +X 8\n");
    for (auto y = 0; y < 2; ++y)
        for (auto x = 0; x < 8; ++x)
        {
            bytes.push_back(byte(u8(128))); // r mantissa
            bytes.push_back(byte(0));
            bytes.push_back(byte(0));
            bytes.push_back(byte(u8(129 + y))); // exponent: row 0 is half of row 1
        }

    auto const decoded = hdr::read(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().stored_bottom_up);

    auto const samples = decoded.value().samples_f32();
    REQUIRE(samples.size() == 48);
    CHECK(samples[0] == 2.0f);         // top row of the image is the file's SECOND scanline
    CHECK(samples[8 * 3 + 0] == 1.0f); // bottom row of the image is the file's first
}

TEST("hdr - the old '1 1 1 n' repeat marker is honored")
{
    // No current writer emits this, ours included, so the only way to cover the path is to build the file.
    auto bytes = cc::vector<byte>();
    append_text(bytes, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n");

    bytes.push_back(byte(u8(128))); // one real pixel: 1.0 in red
    bytes.push_back(byte(0));
    bytes.push_back(byte(0));
    bytes.push_back(byte(u8(129)));

    bytes.push_back(byte(1)); // repeat marker
    bytes.push_back(byte(1));
    bytes.push_back(byte(1));
    bytes.push_back(byte(u8(7))); // ... seven more copies

    auto const decoded = hdr::read(bytes);
    REQUIRE(decoded.has_value());

    auto const samples = decoded.value().samples_f32();
    REQUIRE(samples.size() == 24);
    for (auto x = 0; x < 8; ++x)
    {
        CHECK(samples[x * 3 + 0] == 1.0f);
        CHECK(samples[x * 3 + 1] == 0.0f);
    }
}

TEST("hdr - a chain of repeat markers is rejected before the shift overflows")
{
    // Each `1 1 1 0` marker advances nothing but the shift, so a long enough chain would shift past an int's
    // width and then form a negative run — which the copy loop would write below the row.
    auto bytes = cc::vector<byte>();
    append_text(bytes, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n");

    bytes.push_back(byte(u8(128))); // one real pixel, so a marker has something to repeat
    bytes.push_back(byte(0));
    bytes.push_back(byte(0));
    bytes.push_back(byte(u8(129)));

    for (auto i = 0; i < 4; ++i) // four zero-count markers: shift reaches 32
    {
        bytes.push_back(byte(1));
        bytes.push_back(byte(1));
        bytes.push_back(byte(1));
        bytes.push_back(byte(0));
    }

    bytes.push_back(byte(1)); // ... and one that would apply that shift
    bytes.push_back(byte(1));
    bytes.push_back(byte(1));
    bytes.push_back(byte(u8(255)));

    CHECK(!hdr::read(bytes).has_value());
}

TEST("hdr - round-trips through the read_stream overload")
{
    auto const src = make_gradient(16, 4);
    auto const encoded = hdr::encode(src);
    REQUIRE(encoded.has_value());

    auto adapter = cc::span_read_stream_adapter(cc::span<byte const>(encoded.value()));
    cc::read_stream stream = adapter;
    auto const decoded = hdr::read(stream);
    REQUIRE(decoded.has_value());
    CHECK(round_trip_is_close(src, decoded.value()));
}

TEST("hdr - error paths")
{
    auto const good = hdr::encode(make_gradient(16, 2)).value();

    // not a Radiance file
    byte const garbage[4] = {byte('n'), byte('o'), byte('p'), byte('e')};
    CHECK(hdr::read(cc::span<byte const>(garbage)).has_error());

    // a header without FORMAT cannot say what its bytes mean
    auto no_format = cc::vector<byte>();
    append_text(no_format, "#?RADIANCE\nSOFTWARE=x\n\n-Y 1 +X 8\n");
    CHECK(hdr::read(no_format).has_error());

    // an unterminated header
    auto no_blank_line = cc::vector<byte>();
    append_text(no_blank_line, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n");
    CHECK(hdr::read(no_blank_line).has_error());

    // a FORMAT this codec cannot interpret
    auto bad_format = cc::vector<byte>();
    append_text(bad_format, "#?RADIANCE\nFORMAT=32-bit_rle_wat\n\n-Y 1 +X 8\n");
    CHECK(hdr::read(bad_format).has_error());

    // column-major resolution lines are not read
    auto column_major = cc::vector<byte>();
    append_text(column_major, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+X 8 -Y 1\n");
    CHECK(hdr::read(column_major).has_error());

    // truncated scanline data
    auto truncated = cc::vector<byte>(good);
    truncated.resize_down_to(truncated.size() - 20);
    CHECK(hdr::read(truncated).has_error());

    // encoding rejects what RGBE cannot store
    CHECK(hdr::encode(hdr::data{}).has_error());
    auto four_channels = make_gradient(8, 2);
    four_channels.channels = 4;
    CHECK(hdr::encode(four_channels).has_error());
    auto short_buffer = make_gradient(8, 2);
    short_buffer.width = 16; // the buffer no longer covers what the header claims
    CHECK(hdr::encode(short_buffer).has_error());
    auto bad_software = make_gradient(8, 2);
    bad_software.software = "two\nlines";
    CHECK(hdr::encode(bad_software).has_error());
}
