#include <babel-serializer/image/pfm.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// PFM stores IEEE-754 bits, so unlike every other image codec here the round-trip assertion is EQUALITY.
// That is the property worth pinning: a value that changes has been reinterpreted somewhere.

namespace
{
namespace pfm = babel::pfm;

pfm::data make_gradient(int width, int height, int channels)
{
    auto out = pfm::data{.width = width, .height = height, .channels = channels};
    out.pixels.resize_to_uninitialized(isize(width) * height * channels * isize(sizeof(float)));

    auto* const samples = reinterpret_cast<float*>(out.pixels.data());
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < channels; ++c)
                samples[(isize(y) * width + x) * channels + c] = float(x) * 0.125f - float(y) * 3.5f + float(c);

    return out;
}

bool samples_equal(cc::span<float const> a, cc::span<float const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

void append_text(cc::vector<byte>& out, cc::string_view text)
{
    for (auto const c : text)
        out.push_back(byte(c));
}
} // namespace

TEST("pfm - colour round-trip is bit-exact")
{
    auto const src = make_gradient(5, 3, 3);

    auto const encoded = pfm::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = pfm::read(encoded.value());
    REQUIRE(decoded.has_value());

    auto const& d = decoded.value();
    CHECK(d.width == 5);
    CHECK(d.height == 3);
    CHECK(d.channels == 3);
    CHECK(d.byte_order == pfm::endianness::little);
    CHECK(d.scale == 1.0f);
    CHECK(samples_equal(d.samples_f32(), src.samples_f32()));
}

TEST("pfm - greyscale round-trip is bit-exact")
{
    auto const src = make_gradient(4, 4, 1);

    auto const encoded = pfm::encode(src);
    REQUIRE(encoded.has_value());
    CHECK(encoded.value()[0] == byte('P'));
    CHECK(encoded.value()[1] == byte('f')); // lowercase f is the greyscale magic

    auto const decoded = pfm::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().channels == 1);
    CHECK(samples_equal(decoded.value().samples_f32(), src.samples_f32()));
}

TEST("pfm - big-endian files carry the same values and a positive scale")
{
    auto const src = make_gradient(6, 2, 3);

    auto const little = pfm::encode(src, {.byte_order = pfm::endianness::little});
    auto const big = pfm::encode(src, {.byte_order = pfm::endianness::big});
    REQUIRE(little.has_value());
    REQUIRE(big.has_value());

    // The scale line carries the sign, so the two headers differ by exactly that one '-'.
    CHECK(little.value().size() == big.value().size() + 1);

    // ... and the sample bytes are each other's reverse, four at a time.
    auto const& little_bytes = little.value();
    auto const& big_bytes = big.value();
    CHECK(little_bytes[little_bytes.size() - 1] == big_bytes[big_bytes.size() - 4]);
    CHECK(little_bytes[little_bytes.size() - 4] == big_bytes[big_bytes.size() - 1]);

    auto const decoded = pfm::read(big.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().byte_order == pfm::endianness::big);
    CHECK(samples_equal(decoded.value().samples_f32(), src.samples_f32()));
}

TEST("pfm - rows go out bottom-to-top")
{
    // One column, three rows, so the file's sample order IS the row order.
    auto src = pfm::data{.width = 1, .height = 3, .channels = 1};
    src.pixels.resize_to_uninitialized(3 * isize(sizeof(float)));
    auto* const samples = reinterpret_cast<float*>(src.pixels.data());
    samples[0] = 10.0f; // top row
    samples[1] = 20.0f;
    samples[2] = 30.0f; // bottom row

    auto const encoded = pfm::encode(src);
    REQUIRE(encoded.has_value());

    // The header is "Pf\n1 3\n-1\n": find where the samples start rather than hard-coding its length.
    auto const& bytes = encoded.value();
    auto newlines = 0;
    auto first_sample = isize(0);
    for (auto i = isize(0); i < bytes.size(); ++i)
        if (bytes[i] == byte('\n') && ++newlines == 3)
        {
            first_sample = i + 1;
            break;
        }
    REQUIRE(newlines == 3);
    REQUIRE(bytes.size() - first_sample == 3 * isize(sizeof(float)));

    auto stored = cc::vector<float>();
    stored.resize_to_defaulted(3);
    cc::memcpy(stored.data(), bytes.data() + first_sample, 3 * sizeof(float));
    CHECK(stored[0] == 30.0f); // the file opens with the BOTTOM row
    CHECK(stored[2] == 10.0f);

    auto const decoded = pfm::read(bytes);
    REQUIRE(decoded.has_value());
    CHECK(samples_equal(decoded.value().samples_f32(), src.samples_f32())); // and comes back top-first
}

TEST("pfm - the scale factor is metadata, kept and never applied")
{
    auto src = make_gradient(4, 2, 3);
    src.scale = 4.0f;

    auto const encoded = pfm::encode(src);
    REQUIRE(encoded.has_value());

    auto const decoded = pfm::read(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().scale == 4.0f);
    CHECK(samples_equal(decoded.value().samples_f32(), src.samples_f32())); // samples untouched by the scale
}

TEST("pfm - a hand-written header with spaces and a big-endian sample reads")
{
    // The spec separates the five tokens by "whitespace", not by newlines, and exactly one byte follows the scale.
    auto bytes = cc::vector<byte>();
    append_text(bytes, "Pf 1 1 1.0 ");
    bytes.push_back(byte(0x3F)); // 1.0f, most significant byte first
    bytes.push_back(byte(0x80));
    bytes.push_back(byte(0x00));
    bytes.push_back(byte(0x00));

    auto const decoded = pfm::read(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().byte_order == pfm::endianness::big);
    REQUIRE(decoded.value().samples_f32().size() == 1);
    CHECK(decoded.value().samples_f32()[0] == 1.0f);
}

TEST("pfm - round-trips through the read_stream overload")
{
    auto const src = make_gradient(3, 2, 3);
    auto const encoded = pfm::encode(src);
    REQUIRE(encoded.has_value());

    auto adapter = cc::span_read_stream_adapter(cc::span<byte const>(encoded.value()));
    cc::read_stream stream = adapter;
    auto const decoded = pfm::read(stream);
    REQUIRE(decoded.has_value());
    CHECK(samples_equal(decoded.value().samples_f32(), src.samples_f32()));
}

TEST("pfm - error paths")
{
    // not a PFM
    byte const garbage[6] = {byte('n'), byte('o'), byte('p'), byte('e'), byte('!'), byte(0)};
    CHECK(pfm::read(cc::span<byte const>(garbage)).has_error());

    // a zero scale declares no byte order, so the samples cannot be read
    auto zero_scale = cc::vector<byte>();
    append_text(zero_scale, "Pf\n1 1\n0\n");
    for (auto i = 0; i < 4; ++i)
        zero_scale.push_back(byte(0));
    CHECK(pfm::read(zero_scale).has_error());

    // dimensions that are not numbers
    auto bad_dimensions = cc::vector<byte>();
    append_text(bad_dimensions, "PF\nwide tall\n-1\n");
    CHECK(pfm::read(bad_dimensions).has_error());

    // short sample data
    auto truncated = cc::vector<byte>(pfm::encode(make_gradient(4, 4, 3)).value());
    truncated.resize_down_to(truncated.size() - 4);
    CHECK(pfm::read(truncated).has_error());

    // encoding rejects what PFM cannot store
    CHECK(pfm::encode(pfm::data{}).has_error());
    auto four_channels = make_gradient(4, 2, 3);
    four_channels.channels = 4;
    CHECK(pfm::encode(four_channels).has_error());
    auto zero_scale_image = make_gradient(4, 2, 3);
    zero_scale_image.scale = 0.0f;
    CHECK(pfm::encode(zero_scale_image).has_error());
}
