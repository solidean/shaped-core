#include <babel-serializer/image/impl/stb_backend.hh>
#include <clean-core/string/format.hh>

#include <cstring> // std::memcpy

// The plain stb headers give only the prototypes (the implementation macros live in extern/stb/src/stb.c);
// babel links the vendored `stb` target PRIVATE, so these includes never leave this TU.
#include <stb_image.h>
#include <stb_image_write.h>

namespace babel::impl
{
namespace
{
/// stbi_write_*_to_func sink: append `size` encoded bytes into the cc::vector behind `context`.
void append_to_vector(void* context, void* data, int size)
{
    auto& out = *static_cast<cc::vector<cc::byte>*>(context);
    auto const old = out.size();
    out.resize_to_uninitialized(old + size);
    std::memcpy(out.data() + old, data, size_t(size));
}

/// Validate a pixel buffer against its claimed geometry — shared by both encoders.
cc::result<cc::unit> check_encode_inputs(cc::span<cc::byte const> pixels, int width, int height, int channels)
{
    if (width <= 0 || height <= 0)
        return cc::error(cc::format("image encode: non-positive dimensions {}x{}", width, height));
    if (channels < 1 || channels > 4)
        return cc::error(cc::format("image encode: unsupported channel count {}", channels));

    auto const needed = isize(width) * isize(height) * isize(channels);
    if (pixels.size() < needed)
        return cc::error(cc::format("image encode: pixel buffer too small ({} < {})", pixels.size(), needed));

    return cc::unit{};
}
} // namespace

cc::result<stb_image> stb_decode(cc::span<cc::byte const> bytes, int req_channels)
{
    if (bytes.empty())
        return cc::error("image decode: empty input");

    auto width = 0;
    auto height = 0;
    auto channels_in_file = 0;
    auto* const decoded = stbi_load_from_memory(reinterpret_cast<stbi_uc const*>(bytes.data()), int(bytes.size()), //
                                                &width, &height, &channels_in_file, req_channels);
    if (decoded == nullptr)
        return cc::error(cc::format("image decode failed: {}", stbi_failure_reason()));

    auto const out_channels = req_channels != 0 ? req_channels : channels_in_file;
    auto const count = isize(width) * isize(height) * isize(out_channels);

    auto result = stb_image{.width = width, .height = height, .channels = out_channels};
    result.pixels.resize_to_uninitialized(count);
    std::memcpy(result.pixels.data(), decoded, size_t(count));
    stbi_image_free(decoded); // never hand stb's malloc'd buffer out
    return cc::move(result);
}

cc::result<cc::vector<cc::byte>> stb_encode_png(cc::span<cc::byte const> pixels, int width, int height, int channels)
{
    CC_RETURN_IF_ERROR(check_encode_inputs(pixels, width, height, channels));

    auto out = cc::vector<cc::byte>();
    auto const stride = width * channels; // tightly packed
    auto const ok = stbi_write_png_to_func(&append_to_vector, &out, width, height, channels, pixels.data(), stride);
    if (ok == 0)
        return cc::error("png encode failed");
    return cc::move(out);
}

cc::result<cc::vector<cc::byte>> stb_encode_jpg(cc::span<cc::byte const> pixels,
                                                int width,
                                                int height,
                                                int channels,
                                                int quality)
{
    CC_RETURN_IF_ERROR(check_encode_inputs(pixels, width, height, channels));

    // stb clamps internally, but keep the public contract explicit.
    if (quality < 1 || quality > 100)
        return cc::error(cc::format("jpg encode: quality {} out of range 1..100", quality));

    auto out = cc::vector<cc::byte>();
    auto const ok = stbi_write_jpg_to_func(&append_to_vector, &out, width, height, channels, pixels.data(), quality);
    if (ok == 0)
        return cc::error("jpg encode failed");
    return cc::move(out);
}
} // namespace babel::impl
