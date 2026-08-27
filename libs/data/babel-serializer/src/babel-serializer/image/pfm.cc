#include <babel-serializer/image/pfm.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/string/string_view.hh>

namespace babel::pfm
{
namespace
{
// A file claiming more than this many samples is rejected before anything is allocated.
constexpr isize k_max_samples = isize(1) << 32;

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// One whitespace-delimited header token, plus where the byte after it sits.
struct token_view
{
    cc::string_view text = {};
    isize next = 0;
};

token_view next_token(cc::span<byte const> bytes, isize pos)
{
    auto const* const chars = reinterpret_cast<char const*>(bytes.data());

    while (pos < bytes.size() && is_space(chars[pos]))
        ++pos;

    auto const start = pos;
    while (pos < bytes.size() && !is_space(chars[pos]))
        ++pos;

    return {.text = cc::string_view(chars + start, pos - start), .next = pos};
}

/// Assemble one f32 from four file bytes in the order the header declared.
float float_from_bytes(byte const* src, endianness order)
{
    auto bits = u32(0);
    if (order == endianness::little)
        bits = u32(u8(src[0])) | u32(u8(src[1])) << 8 | u32(u8(src[2])) << 16 | u32(u8(src[3])) << 24;
    else
        bits = u32(u8(src[3])) | u32(u8(src[2])) << 8 | u32(u8(src[1])) << 16 | u32(u8(src[0])) << 24;

    auto value = 0.0f;
    cc::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// The inverse: lay one f32 down in the declared order, whatever the host's own is.
void float_to_bytes(float value, byte* dst, endianness order)
{
    auto bits = u32(0);
    cc::memcpy(&bits, &value, sizeof(bits));

    if (order == endianness::little)
    {
        dst[0] = byte(u8(bits & 0xFF));
        dst[1] = byte(u8(bits >> 8 & 0xFF));
        dst[2] = byte(u8(bits >> 16 & 0xFF));
        dst[3] = byte(u8(bits >> 24 & 0xFF));
    }
    else
    {
        dst[0] = byte(u8(bits >> 24 & 0xFF));
        dst[1] = byte(u8(bits >> 16 & 0xFF));
        dst[2] = byte(u8(bits >> 8 & 0xFF));
        dst[3] = byte(u8(bits & 0xFF));
    }
}

void append_text(cc::vector<byte>& out, cc::string_view text)
{
    auto const old = out.size();
    out.resize_to_uninitialized(old + text.size());
    cc::memcpy(out.data() + old, text.data(), size_t(text.size()));
}
} // namespace

cc::span<float const> data::samples_f32() const
{
    auto const floats = cc::span<byte const>(pixels).try_reinterpret_as<float const>();
    return floats.has_value() ? floats.value() : cc::span<float const>();
}

cc::result<data> read(cc::span<byte const> bytes)
{
    CC_RECORD_SCOPE("pfm.read");

    if (bytes.size() < 2)
        return cc::error("pfm: buffer too small to hold a PFM header");

    auto result = data{};

    auto const magic = next_token(bytes, 0);
    if (magic.text == "PF")
        result.channels = 3;
    else if (magic.text == "Pf")
        result.channels = 1;
    else
        return cc::error(cc::format("pfm: bad magic '{}' (expected 'PF' or 'Pf')", magic.text));

    auto const width_token = next_token(bytes, magic.next);
    auto const height_token = next_token(bytes, width_token.next);
    auto const scale_token = next_token(bytes, height_token.next);

    auto const width = cc::from_string<int>(width_token.text);
    auto const height = cc::from_string<int>(height_token.text);
    auto const scale = cc::from_string<float>(scale_token.text);
    if (!width.has_value() || !height.has_value())
        return cc::error(cc::format("pfm: bad dimensions '{} {}'", width_token.text, height_token.text));
    if (width.value() <= 0 || height.value() <= 0)
        return cc::error(cc::format("pfm: non-positive dimensions {}x{}", width.value(), height.value()));
    if (!scale.has_value())
        return cc::error(cc::format("pfm: bad scale '{}'", scale_token.text));

    // The sign of the scale is the byte order, so a zero scale would leave the samples unreadable.
    if (scale.value() == 0.0f)
        return cc::error("pfm: scale is zero, so the file declares no byte order");

    result.width = width.value();
    result.height = height.value();
    result.byte_order = scale.value() < 0.0f ? endianness::little : endianness::big;
    result.scale = scale.value() < 0.0f ? -scale.value() : scale.value();

    // Exactly one whitespace byte separates the header from the samples, and it belongs to neither.
    if (scale_token.next >= bytes.size() || !is_space(reinterpret_cast<char const*>(bytes.data())[scale_token.next]))
        return cc::error("pfm: header is not followed by a single whitespace byte");
    auto const first_sample = scale_token.next + 1;

    auto const sample_count = isize(result.width) * isize(result.height) * isize(result.channels);
    if (sample_count > k_max_samples)
        return cc::error(cc::format("pfm: implausible image size {}x{}", result.width, result.height));

    auto const sample_bytes = sample_count * isize(sizeof(float));
    if (bytes.size() - first_sample < sample_bytes)
        return cc::error(cc::format("pfm: sample data is short ({} of {} bytes)", //
                                    bytes.size() - first_sample, sample_bytes));

    // PFM rows run bottom-to-top; babel's do not, so the row order is reversed on the way in.
    result.pixels.resize_to_uninitialized(sample_bytes);
    auto* const out = reinterpret_cast<float*>(result.pixels.data());
    auto const row_samples = isize(result.width) * result.channels;

    for (auto y = 0; y < result.height; ++y)
    {
        auto const* const src = bytes.data() + first_sample + isize(y) * row_samples * isize(sizeof(float));
        auto* const dst = out + isize(result.height - 1 - y) * row_samples;
        for (auto i = isize(0); i < row_samples; ++i)
            dst[i] = float_from_bytes(src + i * isize(sizeof(float)), result.byte_order);
    }

    return cc::move(result);
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<byte>> encode(data const& img, write_options opts)
{
    if (img.is_empty())
        return cc::error("pfm encode: empty image");
    if (img.channels != 1 && img.channels != 3)
        return cc::error(cc::format("pfm encode: PFM stores 1 or 3 channels, got {}", img.channels));

    auto const magnitude = img.scale < 0.0f ? -img.scale : img.scale;
    if (!(magnitude > 0.0f) || !(magnitude < 1e38f)) // also rejects NaN and infinity, which have no header spelling
        return cc::error("pfm encode: scale must be a finite non-zero value");

    auto const samples = img.samples_f32();
    auto const sample_count = isize(img.width) * isize(img.height) * isize(img.channels);
    if (samples.size() < sample_count)
        return cc::error(cc::format("pfm encode: pixel buffer too small ({} < {} floats)", //
                                    samples.size(), sample_count));

    auto out = cc::vector<byte>();

    append_text(out, img.channels == 3 ? "PF\n" : "Pf\n");
    append_text(out, cc::format("{} {}\n", img.width, img.height));
    append_text(out, cc::format("{}\n", opts.byte_order == endianness::little ? -magnitude : magnitude));

    auto const row_samples = isize(img.width) * img.channels;
    auto const header_size = out.size();
    out.resize_to_uninitialized(header_size + sample_count * isize(sizeof(float)));

    for (auto y = 0; y < img.height; ++y)
    {
        auto const* const src = samples.data() + isize(img.height - 1 - y) * row_samples;
        auto* const dst = out.data() + header_size + isize(y) * row_samples * isize(sizeof(float));
        for (auto i = isize(0); i < row_samples; ++i)
            float_to_bytes(src[i], dst + i * isize(sizeof(float)), opts.byte_order);
    }

    return cc::move(out);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::pfm
