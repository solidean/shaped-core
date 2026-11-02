#include <babel-serializer/image/webp.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

// babel links the vendored `webp` target PRIVATE, and this is the one TU that includes its headers.
#include <webp/decode.h>
#include <webp/encode.h>

namespace babel::webp
{
namespace
{
// The largest side a WebP can state: its header spends 14 bits on each.
constexpr int k_max_side = 16383;

cc::string_view name_of(WebPEncodingError e)
{
    switch (e)
    {
    case VP8_ENC_OK:
        return "ok";
    case VP8_ENC_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY:
        return "out of memory while flushing the bitstream";
    case VP8_ENC_ERROR_NULL_PARAMETER:
        return "a null parameter";
    case VP8_ENC_ERROR_INVALID_CONFIGURATION:
        return "an invalid configuration";
    case VP8_ENC_ERROR_BAD_DIMENSION:
        return "a bad dimension";
    case VP8_ENC_ERROR_PARTITION0_OVERFLOW:
        return "partition 0 overflowed";
    case VP8_ENC_ERROR_PARTITION_OVERFLOW:
        return "a partition overflowed";
    case VP8_ENC_ERROR_BAD_WRITE:
        return "a bad write";
    case VP8_ENC_ERROR_FILE_TOO_BIG:
        return "the file is too big";
    case VP8_ENC_ERROR_USER_ABORT:
        return "aborted";
    case VP8_ENC_ERROR_LAST:
        break;
    }
    return "an unknown error";
}

/// libwebp's writer callback: append the bytes to the cc::vector behind the picture's custom_ptr.
int append_to_vector(uint8_t const* data, size_t size, WebPPicture const* picture)
{
    auto& out = *static_cast<cc::vector<byte>*>(picture->custom_ptr);
    auto const old = out.size();
    out.resize_to_uninitialized(old + isize(size));
    cc::memcpy(out.data() + old, data, size);
    return 1;
}
} // namespace

cc::result<data> read(cc::span<byte const> bytes)
{
    CC_RECORD_SCOPE("webp.read");
    auto const* const in = reinterpret_cast<uint8_t const*>(bytes.data());
    auto features = WebPBitstreamFeatures{};
    if (WebPGetFeatures(in, size_t(bytes.size()), &features) != VP8_STATUS_OK)
        return cc::error("webp read: not a WebP bitstream libwebp can read");

    auto out = data{.width = features.width,
                    .height = features.height,
                    .channels = features.has_alpha ? 4 : 3,
                    .is_lossless = features.format == 2};
    auto const stride = out.width * out.channels;
    out.pixels.resize_to_uninitialized(isize(stride) * out.height);
    auto* const dst = reinterpret_cast<uint8_t*>(out.pixels.data());
    auto const size = size_t(out.pixels.size());
    auto const* const decoded = features.has_alpha ? WebPDecodeRGBAInto(in, size_t(bytes.size()), dst, size, stride)
                                                   : WebPDecodeRGBInto(in, size_t(bytes.size()), dst, size, stride);
    if (decoded == nullptr)
        return cc::error("webp read: the bitstream does not decode");
    return cc::move(out);
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<byte>> encode(data const& img, write_options opts)
{
    CC_RECORD_SCOPE("webp.encode");
    if (img.is_empty())
        return cc::error("webp encode: empty image");
    if (img.width > k_max_side || img.height > k_max_side)
        return cc::error(cc::format("webp encode: {}x{} is past WebP's {} per side", img.width, img.height, k_max_side));
    if (img.channels < 1 || img.channels > 4)
        return cc::error(cc::format("webp encode: unsupported channel count {}", img.channels));
    auto const count = isize(img.width) * img.height;
    if (img.pixels.size() < count * img.channels)
        return cc::error(
            cc::format("webp encode: pixel buffer too small ({} < {})", img.pixels.size(), count * img.channels));

    // grey widens to RGB, grey + alpha to RGBA: libwebp imports nothing narrower
    auto const has_alpha = img.channels == 2 || img.channels == 4;
    auto widened = cc::vector<byte>();
    auto const* rgb = img.pixels.data();
    if (img.channels <= 2)
    {
        auto const n = has_alpha ? 4 : 3;
        widened.resize_to_uninitialized(count * n);
        for (auto i = isize(0); i < count; ++i)
        {
            auto const* const p = img.pixels.data() + i * img.channels;
            for (auto k = 0; k < 3; ++k)
                widened[i * n + k] = p[0];
            if (has_alpha)
                widened[i * n + 3] = p[1];
        }
        rgb = widened.data();
    }

    auto config = WebPConfig{};
    if (!WebPConfigInit(&config))
        return cc::error("webp encode: libwebp's version does not match its headers");
    config.lossless = opts.lossless ? 1 : 0;
    config.quality = f32(cc::clamp(opts.quality, 0, 100));
    config.method = cc::clamp(opts.method, 0, 6);
    config.exact = opts.exact ? 1 : 0;
    if (!WebPValidateConfig(&config))
        return cc::error("webp encode: an invalid configuration");

    auto picture = WebPPicture{};
    if (!WebPPictureInit(&picture))
        return cc::error("webp encode: libwebp's version does not match its headers");
    picture.width = img.width;
    picture.height = img.height;
    picture.use_argb = opts.lossless ? 1 : 0;
    auto const stride = img.width * (has_alpha ? 4 : 3);
    auto const* const src = reinterpret_cast<uint8_t const*>(rgb);
    auto const imported
        = has_alpha ? WebPPictureImportRGBA(&picture, src, stride) : WebPPictureImportRGB(&picture, src, stride);
    if (!imported)
    {
        WebPPictureFree(&picture);
        return cc::error("webp encode: out of memory importing the pixels");
    }

    auto out = cc::vector<byte>();
    picture.writer = append_to_vector;
    picture.custom_ptr = &out;
    auto const ok = WebPEncode(&config, &picture);
    auto const error = picture.error_code;
    WebPPictureFree(&picture);
    if (!ok)
        return cc::error(cc::format("webp encode: {}", name_of(error)));
    return cc::move(out);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::webp
