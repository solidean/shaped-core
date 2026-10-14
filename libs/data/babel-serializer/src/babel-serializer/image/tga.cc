#include <babel-serializer/image/tga.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>

namespace babel::tga
{
namespace
{
constexpr isize header_size = 18;
constexpr isize footer_size = 26;
constexpr cc::string_view footer_signature = cc::string_view("TRUEVISION-XFILE.\0", 18);

/// The fields of the 18-byte header, as stored.
struct raw_header
{
    int id_length = 0;
    int color_map_type = 0;
    int type_code = 0;
    int color_map_first = 0;
    int color_map_length = 0;
    int color_map_depth = 0;
    int x_origin = 0;
    int y_origin = 0;
    int width = 0;
    int height = 0;
    int pixel_depth = 0;
    int descriptor = 0;
};

raw_header parse_header(cc::span<byte const> bytes)
{
    return {.id_length = int(u8(bytes[0])),
            .color_map_type = int(u8(bytes[1])),
            .type_code = int(u8(bytes[2])),
            .color_map_first = int(cc::load_bytes_le<u16>(bytes, 3)),
            .color_map_length = int(cc::load_bytes_le<u16>(bytes, 5)),
            .color_map_depth = int(u8(bytes[7])),
            .x_origin = int(cc::load_bytes_le<u16>(bytes, 8)),
            .y_origin = int(cc::load_bytes_le<u16>(bytes, 10)),
            .width = int(cc::load_bytes_le<u16>(bytes, 12)),
            .height = int(cc::load_bytes_le<u16>(bytes, 14)),
            .pixel_depth = int(u8(bytes[16])),
            .descriptor = int(u8(bytes[17]))};
}

bool is_color_depth(int bits)
{
    return bits == 15 || bits == 16 || bits == 24 || bits == 32;
}

/// Checks the header against itself, and names the first thing wrong with it.
cc::result<cc::unit> check_header(raw_header const& h)
{
    auto const base_type = h.type_code & ~8;
    if (h.type_code == 0)
        return cc::error("tga: the file declares no image data (type 0)");
    if (base_type < 1 || base_type > 3 || h.type_code > 11)
        return cc::error(cc::format("tga: image type {} is not one the format defines", h.type_code));
    if (h.color_map_type > 1)
        return cc::error(cc::format("tga: color-map type {} is not one the format defines", h.color_map_type));
    if (h.width <= 0 || h.height <= 0)
        return cc::error(cc::format("tga: zero extent {}x{}", h.width, h.height));

    if (h.color_map_type == 1 && !is_color_depth(h.color_map_depth))
        return cc::error(cc::format("tga: a palette entry of {} bits", h.color_map_depth));

    switch (base_type)
    {
    case 1:
        if (h.color_map_type != 1)
            return cc::error("tga: a color-mapped image without a color map");
        if (h.pixel_depth != 8 && h.pixel_depth != 16)
            return cc::error(cc::format("tga: a color-mapped image indexed by {} bits", h.pixel_depth));
        break;
    case 2:
        if (!is_color_depth(h.pixel_depth))
            return cc::error(cc::format("tga: a true-color image of {} bits a pixel", h.pixel_depth));
        break;
    default:
        if (h.pixel_depth != 8 && h.pixel_depth != 16)
            return cc::error(cc::format("tga: a grayscale image of {} bits a pixel", h.pixel_depth));
        break;
    }
    return cc::unit{};
}

/// The channels a stored color of `bits` bits decodes to.
int channels_of_color(int bits, int alpha_bits)
{
    if (bits == 32)
        return 4;
    if (bits == 16 && alpha_bits > 0)
        return 4;
    return 3;
}

/// One stored color (15/16, 24 or 32 bits, blue first) as r, g, b, a.
void decode_color(cc::span<byte const> bytes, isize at, int bits, u8* out)
{
    if (bits == 15 || bits == 16)
    {
        auto const v = u32(cc::load_bytes_le<u16>(bytes, at));
        auto const expand = [](u32 c) { return u8((c << 3) | (c >> 2)); };
        out[0] = expand((v >> 10) & 31u);
        out[1] = expand((v >> 5) & 31u);
        out[2] = expand(v & 31u);
        out[3] = (v & 0x8000u) != 0 ? 255 : 0;
        return;
    }
    out[0] = u8(bytes[at + 2]);
    out[1] = u8(bytes[at + 1]);
    out[2] = u8(bytes[at + 0]);
    out[3] = bits == 32 ? u8(bytes[at + 3]) : 255;
}
} // namespace

bool has_footer(cc::span<byte const> bytes)
{
    if (bytes.size() < header_size + footer_size)
        return false;
    auto const tail = bytes.last_n(footer_signature.size());
    for (auto i = isize(0); i < tail.size(); ++i)
        if (u8(tail[i]) != u8(footer_signature[i]))
            return false;
    return true;
}

bool has_plausible_header(cc::span<byte const> bytes)
{
    if (bytes.size() < header_size)
        return false;
    auto const h = parse_header(bytes);
    // Bits 6 and 7 of the descriptor are the obsolete interleave flags, zero in every file still written.
    if ((h.descriptor & 0xC0) != 0)
        return false;
    if (h.color_map_type == 0 && (h.color_map_length != 0 || h.color_map_depth != 0))
        return false;
    return check_header(h).has_value();
}

cc::result<data> read(cc::span<byte const> bytes, read_options opts)
{
    CC_RECORD_SCOPE("tga.read");

    if (bytes.size() < header_size)
        return cc::error(cc::format("tga: {} bytes cannot hold the {}-byte header", bytes.size(), header_size));

    auto const h = parse_header(bytes);
    CC_RETURN_IF_ERROR(check_header(h));

    auto result = data{.width = h.width,
                       .height = h.height,
                       .type = image_type(((h.type_code & ~8) - 1)),
                       .run_length_encoded = (h.type_code & 8) != 0,
                       .pixel_depth = h.pixel_depth,
                       .color_map_depth = h.color_map_type == 1 ? h.color_map_depth : 0,
                       .alpha_bits = h.descriptor & 0x0F,
                       .stored_bottom_up = (h.descriptor & 0x20) == 0,
                       .stored_right_to_left = (h.descriptor & 0x10) != 0,
                       .x_origin = h.x_origin,
                       .y_origin = h.y_origin};

    auto offset = header_size;
    if (bytes.size() - offset < h.id_length)
        return cc::error("tga: truncated inside the image ID field");
    result.image_id = cc::string::create_copy_of(
        cc::string_view(reinterpret_cast<char const*>(bytes.data() + offset), isize(h.id_length)));
    offset += h.id_length;

    // A true-color or grayscale image may still carry a palette, which nothing reads; it is skipped all the same.
    auto palette = cc::vector<u8>();
    if (h.color_map_type == 1)
    {
        auto const entry_bytes = isize((h.color_map_depth + 7) / 8);
        auto const palette_bytes = entry_bytes * h.color_map_length;
        if (bytes.size() - offset < palette_bytes)
            return cc::error("tga: truncated inside the color map");
        if (result.type == image_type::color_mapped)
        {
            palette.resize_to_uninitialized(isize(h.color_map_length) * 4);
            for (auto i = isize(0); i < h.color_map_length; ++i)
                decode_color(bytes, offset + i * entry_bytes, h.color_map_depth, palette.data() + i * 4);
        }
        offset += palette_bytes;
    }

    switch (result.type)
    {
    case image_type::color_mapped:
        result.channels = channels_of_color(h.color_map_depth, result.alpha_bits);
        break;
    case image_type::true_color:
        result.channels = channels_of_color(h.pixel_depth, result.alpha_bits);
        break;
    case image_type::grayscale:
        result.channels = h.pixel_depth == 16 ? 2 : 1;
        break;
    }

    // A two-byte run-length packet describes up to 128 pixels, so a plausible file can still demand
    // gigabytes; the budget is what bounds the allocation, checked before anything is made.
    auto const pixel_count = isize(h.width) * isize(h.height);
    if (pixel_count * result.channels > opts.max_decoded_bytes)
        return cc::error(cc::format("tga: {}x{} decodes to {} bytes, past the {}-byte budget", //
                                    h.width, h.height, pixel_count * result.channels, opts.max_decoded_bytes));

    auto const stored_bytes = isize((h.pixel_depth + 7) / 8);
    auto const remaining = bytes.size() - offset;
    if (!result.run_length_encoded && remaining < pixel_count * stored_bytes)
        return cc::error(cc::format("tga: pixel data is short ({} of {} bytes)", remaining, pixel_count * stored_bytes));
    // A two-byte packet covers at most 128 pixels, which bounds what an RLE stream of this length can describe.
    if (result.run_length_encoded && remaining * 64 < pixel_count)
        return cc::error(cc::format("tga: {} bytes of run-length data cannot cover {} pixels", remaining, pixel_count));

    auto const channels = result.channels;
    result.pixels.resize_to_uninitialized(pixel_count * channels);
    auto* const out = reinterpret_cast<u8*>(result.pixels.data());

    // Writes the stored pixel at `at` as the `index`-th pixel in file order, flipping into a top-left origin.
    auto const emit = [&](isize index, isize at) -> cc::result<cc::unit>
    {
        auto const sx = index % h.width;
        auto const sy = index / h.width;
        auto const x = result.stored_right_to_left ? h.width - 1 - sx : sx;
        auto const y = result.stored_bottom_up ? h.height - 1 - sy : sy;
        auto* const dst = out + (y * h.width + x) * channels;

        auto rgba = cc::fixed_array<u8, 4>{};
        switch (result.type)
        {
        case image_type::color_mapped:
        {
            auto const raw = h.pixel_depth == 8 ? int(u8(bytes[at])) : int(cc::load_bytes_le<u16>(bytes, at));
            auto const entry = raw - h.color_map_first;
            if (entry < 0 || entry >= h.color_map_length)
                return cc::error(cc::format("tga: palette index {} lies outside the map's {} entries from {}", //
                                            raw, h.color_map_length, h.color_map_first));
            for (auto c = 0; c < 4; ++c)
                rgba[c] = palette[isize(entry) * 4 + c];
            break;
        }
        case image_type::true_color:
            decode_color(bytes, at, h.pixel_depth, rgba.data());
            break;
        case image_type::grayscale:
            dst[0] = u8(bytes[at]);
            if (channels == 2)
                dst[1] = u8(bytes[at + 1]);
            return cc::unit{};
        }
        for (auto c = 0; c < channels; ++c)
            dst[c] = rgba[c];
        return cc::unit{};
    };

    if (!result.run_length_encoded)
    {
        for (auto i = isize(0); i < pixel_count; ++i)
            CC_RETURN_IF_ERROR(emit(i, offset + i * stored_bytes));
        return cc::move(result);
    }

    // Packets may run across rows, which the format allowed before 2.0 and writers still do, so the stream is
    // walked as one run of pixels in file order.
    auto produced = isize(0);
    while (produced < pixel_count)
    {
        if (offset >= bytes.size())
            return cc::error(cc::format("tga: run-length data ends after {} of {} pixels", produced, pixel_count));
        auto const packet = int(u8(bytes[offset]));
        offset += 1;

        auto const count = isize((packet & 0x7F) + 1);
        if (produced + count > pixel_count)
            return cc::error(cc::format("tga: a run-length packet runs {} pixels past the image", //
                                        produced + count - pixel_count));

        auto const repeated = (packet & 0x80) != 0;
        auto const needed = repeated ? stored_bytes : stored_bytes * count;
        if (bytes.size() - offset < needed)
            return cc::error(cc::format("tga: run-length data ends inside a packet after {} pixels", produced));

        for (auto i = isize(0); i < count; ++i)
            CC_RETURN_IF_ERROR(emit(produced + i, repeated ? offset : offset + i * stored_bytes));
        offset += needed;
        produced += count;
    }

    return cc::move(result);
}

cc::result<data> read(cc::read_stream& in, read_options opts)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value(), opts);
}
} // namespace babel::tga
