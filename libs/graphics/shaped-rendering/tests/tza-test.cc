#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/tza.hh>

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
[[nodiscard]] cc::vector<std::byte> load_weights()
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

    // The declared bytes are inside the blob and the largest layer is the size it should be.
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

    std::byte not_an_archive[] = {std::byte(1), std::byte(2), std::byte(3), std::byte(4)};
    CHECK(sr::impl::read_tza(not_an_archive).empty());

    // A well-formed header whose table offset points past the end.
    std::byte header[12] = {};
    header[0] = std::byte(0xD7);
    header[1] = std::byte(0x41);
    header[2] = std::byte(2);
    header[3] = std::byte(0);
    for (auto i = 4; i < 12; ++i)
        header[i] = std::byte(0xFF);
    CHECK(sr::impl::read_tza(header).empty());

    // And a truncated real one: the header survives, the table does not.
    auto const blob = load_weights();
    if (blob.empty())
        return;
    CHECK(sr::impl::read_tza(cc::span<std::byte const>(blob).subspan({.offset = 0, .size = 64})).empty());
}
