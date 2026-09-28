#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/impl/tza.hh>

using namespace cc::primitive_defines;

// The tensor archive OIDN stores its trained network in, and the network it describes.
//
// The shapes matter more than the parse here.
// The denoise member's shaders are written against a FIXED topology — sixteen convolutions, four pools, four skip
// concats — while the layer widths come out of this file.
// So a weights bump that changed either would be a silently wrong image rather than a failure.
// Pinning both is what makes that bump a red test.

namespace
{
/// The pinned blob, or empty when the weights were not fetched into this build.
[[nodiscard]] cc::vector<byte> load_weights()
{
    auto const path = cc::string(SR_OIDN_WEIGHTS_DIR) + "/rt_hdr_alb_nrm.tza";

    // The adapter owns the buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};

    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return {};
    return cc::move(bytes.value());
}

/// A tensor archive written by hand, little-endian like the real one, so a malformed case needs no file on disk.
struct archive_writer
{
    cc::vector<byte> bytes;

    void put_u8(u32 v) { bytes.push_back(byte(v & 0xFF)); }
    void put_u16(u32 v)
    {
        for (auto i = 0; i < 2; ++i)
            put_u8(v >> (8 * i));
    }
    void put_u32(u64 v)
    {
        for (auto i = 0; i < 4; ++i)
            put_u8(u32(v >> (8 * i)));
    }
    void put_u64(u64 v)
    {
        for (auto i = 0; i < 8; ++i)
            put_u8(u32(v >> (8 * i)));
    }
    void put_chars(cc::string_view s)
    {
        for (auto const ch : s)
            put_u8(u32(u8(ch)));
    }

    /// The header, with the table at `table_offset`.
    void header(u64 table_offset)
    {
        put_u16(0x41D7);
        put_u8(2);
        put_u8(0);
        put_u64(table_offset);
    }
};
} // namespace

TEST("sr - the OIDN weights parse into the network the shaders expect")
{
    auto const blob = load_weights();
    if (blob.empty())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    auto const tensors = sr::impl::read_tza(blob);
    REQUIRE(tensors.size() == 32).context(cc::format("{} tensors", tensors.size()));

    // Every layer, with the channel counts the shaders are sized from.
    // `a` layers take a concatenation, so their input width is the upsampled feature count plus the skip's — the
    // arithmetic that has to keep holding is spelled out beside each one.
    struct layer
    {
        char const* name;
        int out_channels;
        int in_channels;
    };
    constexpr layer layers[] = {
        {"enc_conv0", 32, 9}, // 3 radiance + 3 albedo + 3 normal
        {"enc_conv1", 32, 32},    {"enc_conv2", 48, 32},   {"enc_conv3", 64, 48},    {"enc_conv4", 80, 64},
        {"enc_conv5a", 96, 80},   {"enc_conv5b", 96, 96},  {"dec_conv4a", 112, 160}, // 96 upsampled + 64 from pool3
        {"dec_conv4b", 112, 112}, {"dec_conv3a", 96, 160},                           // 112 + 48 from pool2
        {"dec_conv3b", 96, 96},   {"dec_conv2a", 64, 128},                           // 96 + 32 from pool1
        {"dec_conv2b", 64, 64},   {"dec_conv1a", 64, 73},                            // 64 + the 9 input channels
        {"dec_conv1b", 32, 64},   {"dec_conv0", 3, 32},                              // back to radiance
    };

    for (auto const& l : layers)
    {
        auto const* const weight = sr::impl::find_tza(tensors, cc::string(l.name) + ".weight");
        REQUIRE(weight != nullptr).context(cc::format("{}.weight is missing", l.name));
        REQUIRE(weight->dims.size() == 4).context(cc::format("{}.weight is not oihw", l.name));

        CHECK(weight->layout == "oihw").context(cc::format("{}.weight layout is '{}'", l.name, weight->layout));
        CHECK(weight->dims[0] == l.out_channels)
            .context(cc::format("{}.weight has {} output channels", l.name, weight->dims[0]));
        CHECK(weight->dims[1] == l.in_channels)
            .context(cc::format("{}.weight has {} input channels", l.name, weight->dims[1]));

        // 3x3 everywhere, which is what lets one convolution shader serve the whole network.
        CHECK(weight->dims[2] == 3);
        CHECK(weight->dims[3] == 3);

        // Half precision everywhere, which is what the weight upload assumes.
        CHECK(weight->element == sr::impl::tza_element::float16).context(cc::format("{}.weight is not fp16", l.name));

        auto const* const bias = sr::impl::find_tza(tensors, cc::string(l.name) + ".bias");
        REQUIRE(bias != nullptr).context(cc::format("{}.bias is missing", l.name));
        CHECK(bias->dims.size() == 1);
        CHECK(bias->dims[0] == l.out_channels);
        CHECK(bias->element == sr::impl::tza_element::float16);
    }

    // Every tensor's declared bytes are inside the blob and match its shape.
    for (auto const& t : tensors)
        CHECK(t.data.size() == t.element_count() * 2).context(cc::format("{} has {} bytes", t.name, t.data.size()));
}

// A blob that is not one comes back empty rather than reading past its end.
//
// The archive carries an offset to its own table, so the first thing a corrupt file controls is where the reader
// jumps — which is exactly the read this has to refuse.
TEST("sr - a corrupt tensor archive is refused rather than followed")
{
    nx::allow_warnings("tza:");

    CHECK(sr::impl::read_tza({}).empty());

    byte not_an_archive[] = {byte(1), byte(2), byte(3), byte(4)};
    CHECK(sr::impl::read_tza(not_an_archive).empty());

    // A well-formed header whose table offset points past the end.
    byte header[12] = {};
    header[0] = byte(0xD7);
    header[1] = byte(0x41);
    header[2] = byte(2);
    header[3] = byte(0);
    for (auto i = 4; i < 12; ++i)
        header[i] = byte(0xFF);
    CHECK(sr::impl::read_tza(header).empty());

    // A table claiming more tensors than its bytes could describe is refused before anything is sized by it.
    {
        auto w = archive_writer();
        w.header(12);
        w.put_u32(0xFFFFFFFFu);
        CHECK(sr::impl::read_tza(w.bytes).empty());
    }

    // Dimensions whose product overflows any integer are refused rather than wrapped into a small, plausible size.
    {
        auto w = archive_writer();
        w.header(12);
        w.put_u32(1);
        w.put_u16(1);
        w.put_chars("w");
        w.put_u8(3);
        for (auto i = 0; i < 3; ++i)
            w.put_u32(u64(1) << 30);
        w.put_chars("abc");
        w.put_chars("h");
        w.put_u64(0);
        CHECK(sr::impl::read_tza(w.bytes).empty());
    }

    // A well-formed one-tensor archive parses, which is what makes the two refusals above mean something,
    // and every prefix of it short of the whole is refused.
    {
        auto w = archive_writer();
        w.header(12 + 8);
        w.put_u16(0x3C00); // the tensor's data: four fp16 values, 1.0 each
        w.put_u16(0x3C00);
        w.put_u16(0x3C00);
        w.put_u16(0x3C00);
        w.put_u32(1);
        w.put_u16(4);
        w.put_chars("bias");
        w.put_u8(1);
        w.put_u32(4);
        w.put_chars("x");
        w.put_chars("h");
        w.put_u64(12);

        auto const whole = sr::impl::read_tza(w.bytes);
        REQUIRE(whole.size() == 1);
        CHECK(whole[0].name == "bias");
        CHECK(whole[0].element_count() == 4);
        CHECK(whole[0].data.size() == 8);

        for (auto size = 0; size < w.bytes.size(); ++size)
            CHECK(sr::impl::read_tza(cc::span<byte const>(w.bytes).subspan({.offset = 0, .size = size})).empty())
                .context(cc::format("a {}-byte prefix of a {}-byte archive", size, w.bytes.size()));
    }
}

// fp16 widened, against values written out rather than computed by a second implementation.
//
// Subnormals are the case worth pinning: a trained network's smallest weights are exactly those, and getting their
// exponent off by one doubles every one of them without moving the oracle's mean far enough to notice.
TEST("sr - a half widens to the float it encodes")
{
    struct pair
    {
        u16 half = 0;
        f32 value = 0.0f;
    };
    constexpr pair cases[] = {
        {0x0000, 0.0f},
        {0x0001, 5.9604644775390625e-08f}, // 2^-24, the smallest subnormal
        {0x0200, 3.0517578125e-05f},       // 2^-15, a subnormal one shift from normal
        {0x03FF, 6.0975551605224609e-05f}, // the largest subnormal
        {0x0400, 6.103515625e-05f},        // 2^-14, the smallest normal
        {0x3C00, 1.0f},
        {0x3555, 0.333251953125f},
        {0xC000, -2.0f},
        {0x7BFF, 65504.0f}, // the largest finite half
        {0x8001, -5.9604644775390625e-08f},
    };

    for (auto const& c : cases)
        CHECK(sr::impl::half_to_float(c.half) == c.value)
            .context(cc::format("half {} widened to {} rather than {}", c.half, sr::impl::half_to_float(c.half), c.value));

    CHECK(cc::bit_cast<u32>(sr::impl::half_to_float(0x8000)) == 0x80000000u).context("negative zero keeps its sign");
    CHECK(sr::impl::half_to_float(0x7C00) > 3.0e38f).context("infinity");
    auto const nan = sr::impl::half_to_float(0x7E00);
    CHECK(nan != nan).context("NaN");
}
