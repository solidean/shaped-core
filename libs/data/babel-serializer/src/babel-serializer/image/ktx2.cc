#include <babel-serializer/image/ktx2.hh>
#include <clean-core/bytes/compression.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>

namespace babel::ktx2
{
namespace
{
constexpr byte identifier[12] = {byte(0xAB), byte('K'),  byte('T'),  byte('X'),  byte(' '),  byte('2'),
                                 byte('0'),  byte(0xBB), byte('\r'), byte('\n'), byte(0x1A), byte('\n')};

constexpr isize level_index_start = 80;
constexpr isize level_index_stride = 24;

// The data format descriptor's basic block, as Khronos numbers it.
constexpr u8 dfd_transfer_srgb = 2;
constexpr u8 dfd_flag_premultiplied = 1;

// An array may not have more layers than this, far past what any API creates.
constexpr u32 max_layer_count = 1u << 16;

/// What one format stores per texel or block, and what `typeSize` a file of it must state.
struct format_info
{
    int block_bytes = 0;
    int type_size = 1;
};

[[nodiscard]] cc::optional<format_info> info_of(u32 vk_format)
{
    switch (format(vk_format))
    {
    case format::r8_unorm:
    case format::r8_snorm:
    case format::r8_srgb:
        return format_info{.block_bytes = 1, .type_size = 1};
    case format::r8g8_unorm:
    case format::r8g8_snorm:
    case format::r8g8_srgb:
        return format_info{.block_bytes = 2, .type_size = 1};
    case format::r8g8b8a8_unorm:
    case format::r8g8b8a8_snorm:
    case format::r8g8b8a8_srgb:
    case format::b8g8r8a8_unorm:
    case format::b8g8r8a8_srgb:
        return format_info{.block_bytes = 4, .type_size = 1};
    case format::a2b10g10r10_unorm_pack32:
    case format::b10g11r11_ufloat_pack32:
    case format::e5b9g9r9_ufloat_pack32:
        return format_info{.block_bytes = 4, .type_size = 4};
    case format::r16_unorm:
    case format::r16_sfloat:
        return format_info{.block_bytes = 2, .type_size = 2};
    case format::r16g16_unorm:
    case format::r16g16_sfloat:
        return format_info{.block_bytes = 4, .type_size = 2};
    case format::r16g16b16a16_unorm:
    case format::r16g16b16a16_sfloat:
        return format_info{.block_bytes = 8, .type_size = 2};
    case format::r32_sfloat:
        return format_info{.block_bytes = 4, .type_size = 4};
    case format::r32g32_sfloat:
        return format_info{.block_bytes = 8, .type_size = 4};
    case format::r32g32b32a32_sfloat:
        return format_info{.block_bytes = 16, .type_size = 4};
    case format::bc1_rgb_unorm:
    case format::bc1_rgb_srgb:
    case format::bc1_rgba_unorm:
    case format::bc1_rgba_srgb:
    case format::bc4_unorm:
    case format::bc4_snorm:
        return format_info{.block_bytes = 8, .type_size = 1};
    case format::bc2_unorm:
    case format::bc2_srgb:
    case format::bc3_unorm:
    case format::bc3_srgb:
    case format::bc5_unorm:
    case format::bc5_snorm:
    case format::bc6h_ufloat:
    case format::bc6h_sfloat:
    case format::bc7_unorm:
    case format::bc7_srgb:
        return format_info{.block_bytes = 16, .type_size = 1};
    }
    return cc::nullopt;
}

/// The highest level a base of this extent has, counted from 0.
[[nodiscard]] int max_level_of(u32 extent)
{
    auto level = 0;
    while ((extent >> level) > 1)
        ++level;
    return level;
}

/// The data format descriptor's transfer function and flags, where its first block is Khronos's basic one.
void read_dfd(cc::span<byte const> in, header const& h, data& out)
{
    if (h.dfd_byte_length < 16)
        return;
    auto const at = isize(h.dfd_byte_offset);
    auto const vendor_and_type = cc::load_bytes_le<u32>(in, at + 4);
    if (vendor_and_type != 0) // vendor 0 (Khronos), descriptor type 0 (basic)
        return;
    out.is_srgb_transfer = u8(in[at + 14]) == dfd_transfer_srgb;
    out.is_premultiplied_alpha = (u8(in[at + 15]) & dfd_flag_premultiplied) != 0;
}

/// Every key/value entry: a length, the key and its NUL, the value, then padding to 4 bytes.
[[nodiscard]] cc::result<cc::unit> read_kvd(cc::pinned_data<byte const> const& bytes, header const& h, data& out)
{
    auto const in = bytes.span();
    auto at = isize(h.kvd_byte_offset);
    auto const end = at + isize(h.kvd_byte_length);
    while (at + 4 <= end)
    {
        auto const length = isize(cc::load_bytes_le<u32>(in, at));
        auto const entry = at + 4;
        if (length > end - entry)
            return cc::error(cc::format("ktx2: a key/value entry of {} bytes runs past the key/value data", length));

        auto key_end = entry;
        while (key_end < entry + length && in[key_end] != byte(0))
            ++key_end;
        if (key_end == entry + length)
            return cc::error("ktx2: a key/value entry's key has no terminating NUL");

        out.key_values.push_back({
            .key = cc::string(cc::string_view(reinterpret_cast<char const*>(in.data() + entry), key_end - entry)),
            .value = bytes.subdata(cc::start_end{.start = key_end + 1, .end = entry + length}),
        });
        at = entry + ((length + 3) & ~isize(3));
    }
    return cc::unit{};
}

[[nodiscard]] cc::result<header> read_header(cc::span<byte const> in)
{
    if (in.size() < level_index_start)
        return cc::error(cc::format("ktx2: {} bytes cannot hold the {}-byte header", in.size(), level_index_start));
    if (!has_identifier(in))
        return cc::error("ktx2: missing the KTX 2.0 identifier");

    auto h = header();
    h.vk_format = cc::load_bytes_le<u32>(in, 12);
    h.type_size = cc::load_bytes_le<u32>(in, 16);
    h.pixel_width = cc::load_bytes_le<u32>(in, 20);
    h.pixel_height = cc::load_bytes_le<u32>(in, 24);
    h.pixel_depth = cc::load_bytes_le<u32>(in, 28);
    h.layer_count = cc::load_bytes_le<u32>(in, 32);
    h.face_count = cc::load_bytes_le<u32>(in, 36);
    h.level_count = cc::load_bytes_le<u32>(in, 40);
    h.scheme = supercompression(cc::load_bytes_le<u32>(in, 44));
    h.dfd_byte_offset = cc::load_bytes_le<u32>(in, 48);
    h.dfd_byte_length = cc::load_bytes_le<u32>(in, 52);
    h.kvd_byte_offset = cc::load_bytes_le<u32>(in, 56);
    h.kvd_byte_length = cc::load_bytes_le<u32>(in, 60);
    h.sgd_byte_offset = cc::load_bytes_le<u64>(in, 64);
    h.sgd_byte_length = cc::load_bytes_le<u64>(in, 72);

    if (h.scheme == supercompression::basis_lz || h.vk_format == 0)
        return cc::error("ktx2: Basis Universal needs a transcoder, which babel does not carry");
    if (h.scheme != supercompression::none && h.scheme != supercompression::zstd && h.scheme != supercompression::zlib)
        return cc::error(cc::format("ktx2: supercompression scheme {} is not one babel reads", u32(h.scheme)));

    auto const info = info_of(h.vk_format);
    if (!info.has_value())
        return cc::error(cc::format("ktx2: vkFormat {} is not one babel reads", h.vk_format));
    if (h.type_size != u32(info.value().type_size))
        return cc::error(cc::format("ktx2: typeSize is {}, where {} requires {}", h.type_size,
                                    to_string(format(h.vk_format)), info.value().type_size));

    if (h.pixel_width == 0)
        return cc::error("ktx2: pixelWidth is 0");
    if (h.pixel_height == 0 && h.pixel_depth != 0)
        return cc::error("ktx2: a 3D texture must have a height");
    if (h.pixel_width > (1u << 16) || h.pixel_height > (1u << 16) || h.pixel_depth > (1u << 16))
        return cc::error(cc::format("ktx2: {} x {} x {} is past the 65536 texels an axis may have", h.pixel_width,
                                    h.pixel_height, h.pixel_depth));
    if (is_block_compressed(format(h.vk_format)) && (h.pixel_height == 0 || h.pixel_depth != 0))
        return cc::error("ktx2: a block-compressed texture must be 2D");
    if (h.face_count != 1 && h.face_count != 6)
        return cc::error(cc::format("ktx2: faceCount is {}, where only 1 and 6 are allowed", h.face_count));
    if (h.face_count == 6 && (h.pixel_width != h.pixel_height || h.pixel_depth != 0))
        return cc::error("ktx2: a cube map must be square and 2D");
    if (h.layer_count > max_layer_count)
        return cc::error(cc::format("ktx2: layerCount {} is past the {} babel reads", h.layer_count, max_layer_count));

    auto const max_level = max_level_of(cc::max(h.pixel_width, cc::max(h.pixel_height, h.pixel_depth)));
    if (h.level_count > u32(max_level + 1))
        return cc::error(cc::format("ktx2: levelCount {} is more than the {} levels a {} x {} x {} texture has",
                                    h.level_count, max_level + 1, h.pixel_width, h.pixel_height, h.pixel_depth));

    auto const level_count = cc::max(h.level_count, 1u);
    if (isize(level_count) * level_index_stride > in.size() - level_index_start)
        return cc::error(cc::format("ktx2: {} bytes cannot hold the index of {} levels", in.size(), level_count));
    for (auto l = isize(0); l < level_count; ++l)
    {
        auto const at = level_index_start + l * level_index_stride;
        h.levels.push_back({
            .byte_offset = cc::load_bytes_le<u64>(in, at),
            .byte_length = cc::load_bytes_le<u64>(in, at + 8),
            .uncompressed_byte_length = cc::load_bytes_le<u64>(in, at + 16),
        });
    }

    auto const size = u64(in.size());
    auto const inside = [&](u64 offset, u64 length) { return offset <= size && length <= size - offset; };
    if (!inside(h.dfd_byte_offset, h.dfd_byte_length))
        return cc::error("ktx2: the data format descriptor runs past the end of the file");
    if (!inside(h.kvd_byte_offset, h.kvd_byte_length))
        return cc::error("ktx2: the key/value data runs past the end of the file");
    if (!inside(h.sgd_byte_offset, h.sgd_byte_length))
        return cc::error("ktx2: the supercompression global data runs past the end of the file");
    for (auto l = isize(0); l < h.levels.size(); ++l)
        if (!inside(h.levels[l].byte_offset, h.levels[l].byte_length))
            return cc::error(cc::format("ktx2: level {} runs past the end of the file", l));
    return h;
}

/// The bytes level `l` holds once decoded: every layer, every face, every slice.
[[nodiscard]] i64 level_byte_size(header const& h, int l)
{
    auto const w = mip_extent(int(h.pixel_width), l);
    auto const ht = mip_extent(int(cc::max(h.pixel_height, 1u)), l);
    auto const d = mip_extent(int(cc::max(h.pixel_depth, 1u)), l);
    return image_byte_size(format(h.vk_format), w, ht, d) * i64(cc::max(h.layer_count, 1u)) * i64(h.face_count);
}

/// Level `l` as stored, decoded into a buffer of its own; an error names the level.
[[nodiscard]] cc::result<cc::pinned_data<byte const>> decode_level(cc::span<byte const> in, header const& h, int l)
{
    auto const& level = h.levels[l];
    auto const stored = in.subspan({.offset = isize(level.byte_offset), .size = isize(level.byte_length)});
    auto out = cc::pinned_data<byte>::create_uninitialized(isize(level.uncompressed_byte_length));

    auto cfg = cc::decompression_config{.algorithm = cc::compression_algorithm::zstd};
    if (h.scheme == supercompression::zlib)
        cfg = {.algorithm = cc::compression_algorithm::deflate, .framing = cc::compression_framing::zlib};

    auto const written = cc::decompress_into(stored, out.span(), cfg);
    if (written.has_error())
        return cc::error(cc::format("ktx2: level {} does not decode: {}", l, written.error().to_string()));
    if (written.value() != out.size())
        return cc::error(
            cc::format("ktx2: level {} decodes to {} bytes, where its index says {}", l, written.value(), out.size()));
    return out.as_bytes();
}

/// Every level's decoded bytes, on `opts.scheduler` when there is one; the first error by level order wins.
[[nodiscard]] cc::result<cc::vector<cc::pinned_data<byte const>>> decode_levels(cc::span<byte const> in,
                                                                                header const& h,
                                                                                read_options const& opts)
{
    auto const count = h.levels.size();
    auto results = cc::vector<cc::result<cc::pinned_data<byte const>>>();
    for (auto l = isize(0); l < count; ++l)
        results.push_back(cc::error("ktx2: level was not decoded"));

    if (opts.scheduler == nullptr)
    {
        for (auto l = isize(0); l < count; ++l)
            results[l] = decode_level(in, h, int(l));
    }
    else
    {
        auto tasks = cc::vector<cc::shared_async<cc::unit>>();
        tasks.reserve(count);
        for (auto l = isize(0); l < count; ++l)
        {
            auto task = cc::make_async_lazy(
                [&results, in, &h, l]
                {
                    results[l] = decode_level(in, h, int(l));
                    return cc::unit{};
                });
            task->schedule_on(*opts.scheduler);
            tasks.push_back(cc::move(task));
        }
        for (auto const& task : tasks)
            opts.scheduler->participate_until_ready(*task);
    }

    auto levels = cc::vector<cc::pinned_data<byte const>>();
    levels.reserve(count);
    for (auto& r : results)
    {
        if (r.has_error())
            return cc::error(cc::move(r).error());
        levels.push_back(cc::move(r).value());
    }
    return levels;
}
} // namespace

bool is_block_compressed(format fmt)
{
    return u32(fmt) >= u32(format::bc1_rgb_unorm) && u32(fmt) <= u32(format::bc7_srgb);
}

bool is_float(format fmt)
{
    switch (fmt)
    {
    case format::r16_sfloat:
    case format::r16g16_sfloat:
    case format::r16g16b16a16_sfloat:
    case format::r32_sfloat:
    case format::r32g32_sfloat:
    case format::r32g32b32a32_sfloat:
    case format::b10g11r11_ufloat_pack32:
    case format::e5b9g9r9_ufloat_pack32:
    case format::bc6h_ufloat:
    case format::bc6h_sfloat:
        return true;
    default:
        return false;
    }
}

bool is_srgb(format fmt)
{
    switch (fmt)
    {
    case format::r8_srgb:
    case format::r8g8_srgb:
    case format::r8g8b8a8_srgb:
    case format::b8g8r8a8_srgb:
    case format::bc1_rgb_srgb:
    case format::bc1_rgba_srgb:
    case format::bc2_srgb:
    case format::bc3_srgb:
    case format::bc7_srgb:
        return true;
    default:
        return false;
    }
}

int block_byte_size(format fmt)
{
    auto const info = info_of(u32(fmt));
    return info.has_value() ? info.value().block_bytes : 0;
}

i64 image_byte_size(format fmt, int width, int height, int depth)
{
    auto const bytes = i64(block_byte_size(fmt));
    if (is_block_compressed(fmt))
        return i64((width + 3) / 4) * i64((height + 3) / 4) * i64(depth) * bytes;
    return i64(width) * i64(height) * i64(depth) * bytes;
}

bool is_known(u32 vk_format)
{
    return info_of(vk_format).has_value();
}

cc::string_view to_string(format fmt)
{
    switch (fmt)
    {
    case format::r8_unorm:
        return "r8_unorm";
    case format::r8_snorm:
        return "r8_snorm";
    case format::r8_srgb:
        return "r8_srgb";
    case format::r8g8_unorm:
        return "r8g8_unorm";
    case format::r8g8_snorm:
        return "r8g8_snorm";
    case format::r8g8_srgb:
        return "r8g8_srgb";
    case format::r8g8b8a8_unorm:
        return "r8g8b8a8_unorm";
    case format::r8g8b8a8_snorm:
        return "r8g8b8a8_snorm";
    case format::r8g8b8a8_srgb:
        return "r8g8b8a8_srgb";
    case format::b8g8r8a8_unorm:
        return "b8g8r8a8_unorm";
    case format::b8g8r8a8_srgb:
        return "b8g8r8a8_srgb";
    case format::a2b10g10r10_unorm_pack32:
        return "a2b10g10r10_unorm_pack32";
    case format::r16_unorm:
        return "r16_unorm";
    case format::r16_sfloat:
        return "r16_sfloat";
    case format::r16g16_unorm:
        return "r16g16_unorm";
    case format::r16g16_sfloat:
        return "r16g16_sfloat";
    case format::r16g16b16a16_unorm:
        return "r16g16b16a16_unorm";
    case format::r16g16b16a16_sfloat:
        return "r16g16b16a16_sfloat";
    case format::r32_sfloat:
        return "r32_sfloat";
    case format::r32g32_sfloat:
        return "r32g32_sfloat";
    case format::r32g32b32a32_sfloat:
        return "r32g32b32a32_sfloat";
    case format::b10g11r11_ufloat_pack32:
        return "b10g11r11_ufloat_pack32";
    case format::e5b9g9r9_ufloat_pack32:
        return "e5b9g9r9_ufloat_pack32";
    case format::bc1_rgb_unorm:
        return "bc1_rgb_unorm";
    case format::bc1_rgb_srgb:
        return "bc1_rgb_srgb";
    case format::bc1_rgba_unorm:
        return "bc1_rgba_unorm";
    case format::bc1_rgba_srgb:
        return "bc1_rgba_srgb";
    case format::bc2_unorm:
        return "bc2_unorm";
    case format::bc2_srgb:
        return "bc2_srgb";
    case format::bc3_unorm:
        return "bc3_unorm";
    case format::bc3_srgb:
        return "bc3_srgb";
    case format::bc4_unorm:
        return "bc4_unorm";
    case format::bc4_snorm:
        return "bc4_snorm";
    case format::bc5_unorm:
        return "bc5_unorm";
    case format::bc5_snorm:
        return "bc5_snorm";
    case format::bc6h_ufloat:
        return "bc6h_ufloat";
    case format::bc6h_sfloat:
        return "bc6h_sfloat";
    case format::bc7_unorm:
        return "bc7_unorm";
    case format::bc7_srgb:
        return "bc7_srgb";
    }
    return "unknown";
}

bool has_identifier(cc::span<byte const> bytes)
{
    if (bytes.size() < isize(sizeof(identifier)))
        return false;
    for (auto i = 0; i < isize(sizeof(identifier)); ++i)
        if (bytes[i] != identifier[i])
            return false;
    return true;
}

subresource const* data::find(int layer, int face, int mip) const
{
    for (auto const& s : subresources)
        if (s.layer == layer && s.face == face && s.mip == mip)
            return &s;
    return nullptr;
}

key_value const* data::find_key(cc::string_view key) const
{
    for (auto const& kv : key_values)
        if (kv.key == key)
            return &kv;
    return nullptr;
}

cc::result<data> read(cc::pinned_data<byte const> const& bytes, read_options opts)
{
    CC_RECORD_SCOPE("ktx2.read");
    auto const in = bytes.span();

    auto h = read_header(in);
    if (h.has_error())
        return cc::error(cc::move(h).error());

    auto out = data();
    out.file_header = cc::move(h).value();
    auto const& fh = out.file_header;
    out.pixel_format = format(fh.vk_format);
    out.width = int(fh.pixel_width);
    out.height = int(cc::max(fh.pixel_height, 1u));
    out.depth = int(cc::max(fh.pixel_depth, 1u));
    out.mip_count = int(fh.levels.size());
    out.layer_count = int(cc::max(fh.layer_count, 1u));
    out.face_count = int(fh.face_count);

    read_dfd(in, fh, out);
    if (auto kvd = read_kvd(bytes, fh, out); kvd.has_error())
        return cc::error(cc::move(kvd).error());

    // Every level must hold exactly what its extent and format imply, once decoded.
    auto decoded_total = i64(0);
    for (auto l = 0; l < out.mip_count; ++l)
    {
        auto const& level = fh.levels[l];
        auto const want = level_byte_size(fh, l);
        if (fh.scheme == supercompression::none)
        {
            if (level.byte_length != u64(want))
                return cc::error(cc::format("ktx2: level {} stores {} bytes, where a {} level of it holds {}", l,
                                            level.byte_length, to_string(out.pixel_format), want));
        }
        else
        {
            if (level.uncompressed_byte_length != u64(want))
                return cc::error(cc::format("ktx2: level {} decodes to {} bytes by its index, where a {} level of it "
                                            "holds {}",
                                            l, level.uncompressed_byte_length, to_string(out.pixel_format), want));
            decoded_total += want;
            if (decoded_total > opts.max_decoded_bytes)
                return cc::error(
                    cc::format("ktx2: decoding takes more than the {} bytes this read allows", opts.max_decoded_bytes));
        }
    }

    // A plain level is a view of the input; a supercompressed one is decoded, independently of every other.
    auto levels = cc::vector<cc::pinned_data<byte const>>();
    if (fh.scheme == supercompression::none)
    {
        for (auto const& level : fh.levels)
            levels.push_back(bytes.subdata({.offset = isize(level.byte_offset), .size = isize(level.byte_length)}));
    }
    else
    {
        auto decoded = decode_levels(in, fh, opts);
        if (decoded.has_error())
            return cc::error(cc::move(decoded).error());
        levels = cc::move(decoded).value();
    }

    // Within a level: layer by layer, each layer's faces, each face's slices.
    for (auto l = 0; l < out.mip_count; ++l)
    {
        auto const w = mip_extent(out.width, l);
        auto const ht = mip_extent(out.height, l);
        auto const d = mip_extent(out.depth, l);
        auto const image_size = isize(image_byte_size(out.pixel_format, w, ht, d));
        for (auto layer = 0; layer < out.layer_count; ++layer)
            for (auto face = 0; face < out.face_count; ++face)
            {
                auto const at = (isize(layer) * out.face_count + face) * image_size;
                out.subresources.push_back({
                    .mip = l,
                    .layer = layer,
                    .face = face,
                    .width = w,
                    .height = ht,
                    .depth = d,
                    .bytes = levels[l].subdata({.offset = at, .size = image_size}),
                });
            }
    }
    return out;
}

cc::result<data> read(cc::read_stream& in, read_options opts)
{
    auto slurped = in.read_all();
    if (slurped.has_error())
        return cc::error(cc::move(slurped).error());
    // make_pinned_data moves the slurped vector into the pin, so the read is the only copy.
    return read(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(slurped).value())), opts);
}

cc::result<data> read(cc::span<byte const> bytes, read_options opts)
{
    return read(cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytes)), opts);
}

cc::shared_async<data> read_async(cc::pinned_data<byte const> bytes, read_options opts)
{
    auto& scheduler = opts.scheduler != nullptr ? *opts.scheduler : cc::compute_scheduler();
    opts.scheduler = &scheduler;

    auto node = cc::make_async_lazy<data>(
        [bytes = cc::move(bytes), opts](cc::async_context<data>& ctx) -> cc::async_step_status
        {
            auto read = babel::ktx2::read(bytes, opts);
            if (read.has_error())
                return ctx.resolve_to_error(cc::async_error::make_error(cc::move(read).error()));
            return ctx.resolve_to_value(cc::move(read).value());
        });
    node->schedule_on(scheduler);
    return node;
}
} // namespace babel::ktx2
