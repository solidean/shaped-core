#include <babel-serializer/image/ktx2.hh>
#include <clean-core/bytes/compression.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
namespace ktx2 = babel::ktx2;

/// Everything a test varies in a KTX2 file; the defaults are one 4x4 RGBA8 level, stored plain.
struct ktx2_spec
{
    u32 vk_format = u32(ktx2::format::r8g8b8a8_unorm);
    u32 type_size = 1;
    u32 width = 4;
    u32 height = 4;
    u32 depth = 0;
    u32 layers = 0;
    u32 faces = 1;
    u32 levels = 1;
    ktx2::supercompression scheme = ktx2::supercompression::none;

    u8 transfer = 1; // linear
    u8 dfd_flags = 0;
    cc::vector<cc::pair<cc::string, cc::string>> key_values;

    /// Overrides a level's uncompressed byte length in the index; -1 keeps the true one.
    i64 claimed_uncompressed = -1;
};

/// What the file stores for level `l`: `payload(l)` as is, or compressed by the spec's scheme.
cc::vector<byte> stored_level(ktx2_spec const& spec, cc::span<byte const> level)
{
    if (spec.scheme == ktx2::supercompression::zstd)
        return cc::compress(level, {.algorithm = cc::compression_algorithm::zstd});
    if (spec.scheme == ktx2::supercompression::zlib)
        return cc::compress(
            level, {.algorithm = cc::compression_algorithm::deflate, .framing = cc::compression_framing::zlib});
    return cc::vector<byte>::create_copy_of(level);
}

void append_u32(cc::vector<byte>& out, u32 v)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(byte((v >> (8 * i)) & 0xFF));
}

/// A KTX2 file: the header `spec` describes, levels stored smallest first as writers usually do.
/// `levels[l]` is level l's whole decoded payload.
cc::vector<byte> make_ktx2(ktx2_spec const& spec, cc::span<cc::vector<byte> const> levels)
{
    // a basic data format descriptor block, reduced to what babel reads: its header and four bytes of color info
    auto dfd = cc::vector<byte>();
    append_u32(dfd, 28);                 // dfdTotalSize
    append_u32(dfd, 0);                  // vendor 0, descriptor type 0
    append_u32(dfd, 2u | (24u << 16));   // version 2, block size 24
    dfd.push_back(byte(1));              // color model RGBSDA
    dfd.push_back(byte(1));              // primaries BT.709
    dfd.push_back(byte(spec.transfer));  // transfer function
    dfd.push_back(byte(spec.dfd_flags)); // flags
    for (auto i = 0; i < 12; ++i)
        dfd.push_back(byte(0));

    auto kvd = cc::vector<byte>();
    for (auto const& [key, value] : spec.key_values)
    {
        auto const length = u32(key.size() + 1 + value.size() + 1);
        append_u32(kvd, length);
        for (auto const b : key.as_bytes())
            kvd.push_back(b);
        kvd.push_back(byte(0));
        for (auto const b : value.as_bytes())
            kvd.push_back(b);
        kvd.push_back(byte(0));
        while (kvd.size() % 4 != 0)
            kvd.push_back(byte(0));
    }

    auto const level_count = isize(levels.size());
    auto const dfd_at = isize(80) + level_count * 24;
    auto const kvd_at = dfd_at + dfd.size();
    auto out = cc::vector<byte>::create_filled(kvd_at + kvd.size(), byte(0));

    constexpr u8 id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    for (auto i = 0; i < 12; ++i)
        out[i] = byte(id[i]);
    cc::store_bytes_le<u32>(out, 12, spec.vk_format);
    cc::store_bytes_le<u32>(out, 16, spec.type_size);
    cc::store_bytes_le<u32>(out, 20, spec.width);
    cc::store_bytes_le<u32>(out, 24, spec.height);
    cc::store_bytes_le<u32>(out, 28, spec.depth);
    cc::store_bytes_le<u32>(out, 32, spec.layers);
    cc::store_bytes_le<u32>(out, 36, spec.faces);
    cc::store_bytes_le<u32>(out, 40, spec.levels);
    cc::store_bytes_le<u32>(out, 44, u32(spec.scheme));
    cc::store_bytes_le<u32>(out, 48, u32(dfd_at));
    cc::store_bytes_le<u32>(out, 52, u32(dfd.size()));
    cc::store_bytes_le<u32>(out, 56, u32(kvd.size() > 0 ? kvd_at : 0));
    cc::store_bytes_le<u32>(out, 60, u32(kvd.size()));
    for (auto i = isize(0); i < dfd.size(); ++i)
        out[dfd_at + i] = dfd[i];
    for (auto i = isize(0); i < kvd.size(); ++i)
        out[kvd_at + i] = kvd[i];

    for (auto l = level_count - 1; l >= 0; --l)
    {
        auto const stored = stored_level(spec, levels[l]);
        auto const at = out.size();
        for (auto const b : stored)
            out.push_back(b);
        auto const index = 80 + l * 24;
        cc::store_bytes_le<u64>(out, index, u64(at));
        cc::store_bytes_le<u64>(out, index + 8, u64(stored.size()));
        auto const uncompressed = spec.claimed_uncompressed >= 0 ? u64(spec.claimed_uncompressed) : u64(levels[l].size());
        cc::store_bytes_le<u64>(out, index + 16, uncompressed);
    }
    return out;
}

/// `size` bytes counting up from `seed`, so a view of the wrong bytes shows.
cc::vector<byte> counting(isize size, int seed)
{
    auto out = cc::vector<byte>();
    for (auto i = isize(0); i < size; ++i)
        out.push_back(byte((seed + i) & 0xFF));
    return out;
}

bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

cc::string error_of(cc::result<ktx2::data> const& r)
{
    return r.has_error() ? r.error().to_string() : cc::string();
}
} // namespace

// Reading
// -------------------------------------------------------------------------------------------------

TEST("babel::ktx2 - a plain BC7 file's levels are views of the input, base level first")
{
    // 8x8 has four levels: 2x2 blocks, then one block each for 4x4, 2x2 and 1x1
    auto const levels
        = cc::vector<cc::vector<byte>>{counting(64, 0), counting(16, 100), counting(16, 150), counting(16, 200)};
    auto const file = make_ktx2({.vk_format = u32(ktx2::format::bc7_srgb), .width = 8, .height = 8, .levels = 4}, levels);
    auto const pinned = cc::pinned_data<byte const>(cc::make_pinned_data(cc::vector<byte>::create_copy_of(file)));

    auto const tex = REQUIRED_VALUE(ktx2::read(pinned));
    CHECK(tex.pixel_format == ktx2::format::bc7_srgb);
    CHECK(ktx2::is_block_compressed(tex.pixel_format));
    CHECK(ktx2::is_srgb(tex.pixel_format));
    CHECK(tex.width == 8);
    CHECK(tex.height == 8);
    CHECK(tex.mip_count == 4);
    REQUIRE(tex.subresources.size() == 4);

    for (auto l = 0; l < 4; ++l)
    {
        auto const* const s = tex.find(0, 0, l);
        REQUIRE(s != nullptr);
        CHECK(s->width == ktx2::mip_extent(8, l));
        REQUIRE(s->bytes.size() == levels[l].size());
        CHECK(same_bytes(s->bytes.span(), levels[l]));
        // zero-copy: the view points into the input and keeps it alive
        CHECK(s->bytes.data() >= pinned.data());
        CHECK(s->bytes.data() < pinned.data() + pinned.size());
    }
}

TEST("babel::ktx2 - a zstd cube array decodes, and finds each layer, face and level")
{
    // 2 layers of 6 faces, 2x2 RGBA8 with two levels: each image is 16 bytes, then 4
    auto const images = isize(2 * 6);
    auto const levels = cc::vector<cc::vector<byte>>{counting(images * 16, 0), counting(images * 4, 7)};
    auto const file = make_ktx2(
        {.width = 2, .height = 2, .layers = 2, .faces = 6, .levels = 2, .scheme = ktx2::supercompression::zstd}, levels);

    auto const tex = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(file)));
    CHECK(tex.is_cube());
    CHECK(tex.is_array());
    CHECK(tex.layer_count == 2);
    REQUIRE(tex.subresources.size() == 2 * images);

    for (auto l = 0; l < 2; ++l)
        for (auto layer = 0; layer < 2; ++layer)
            for (auto face = 0; face < 6; ++face)
            {
                auto const* const s = tex.find(layer, face, l);
                REQUIRE(s != nullptr);
                auto const image_size = l == 0 ? isize(16) : isize(4);
                auto const at = (isize(layer) * 6 + face) * image_size;
                CHECK(same_bytes(s->bytes.span(),
                                 cc::span<byte const>(levels[l]).subspan({.offset = at, .size = image_size})))
                    .dump("level", l)
                    .dump("layer", layer)
                    .dump("face", face);
            }
    CHECK(tex.find(2, 0, 0) == nullptr);
}

TEST("babel::ktx2 - zlib supercompression decodes like zstd")
{
    auto const levels = cc::vector<cc::vector<byte>>{counting(64, 3)};
    auto const file = make_ktx2({.scheme = ktx2::supercompression::zlib}, levels);
    auto const tex = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(file)));
    REQUIRE(tex.subresources.size() == 1);
    CHECK(same_bytes(tex.subresources[0].bytes.span(), levels[0]));
}

TEST("babel::ktx2 - a 1D and a 3D texture report a height and depth of 1 where the header stores 0")
{
    auto const line = REQUIRED_VALUE(ktx2::read(
        cc::span<byte const>(make_ktx2({.width = 8, .height = 0}, cc::vector<cc::vector<byte>>{counting(32, 0)}))));
    CHECK(line.height == 1);
    CHECK(line.depth == 1);

    auto const volume = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(
        make_ktx2({.width = 2, .height = 2, .depth = 3}, cc::vector<cc::vector<byte>>{counting(2 * 2 * 3 * 4, 0)}))));
    CHECK(volume.depth == 3);
    REQUIRE(volume.subresources.size() == 1);
    CHECK(volume.subresources[0].depth == 3);
}

TEST("babel::ktx2 - the descriptor's sRGB transfer and premultiplied flag, and the key/value entries, are read")
{
    auto const file = make_ktx2({.transfer = 2,
                                 .dfd_flags = 1,
                                 .key_values = {{cc::string("KTXorientation"), cc::string("rd")},
                                                {cc::string("KTXwriter"), cc::string("babel test")}}},
                                cc::vector<cc::vector<byte>>{counting(64, 0)});
    auto const tex = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(file)));
    CHECK(tex.is_srgb_transfer);
    CHECK(tex.is_premultiplied_alpha);
    REQUIRE(tex.key_values.size() == 2);

    auto const* const orientation = tex.find_key("KTXorientation");
    REQUIRE(orientation != nullptr);
    // a text value keeps its terminating NUL
    REQUIRE(orientation->value.size() == 3);
    CHECK(char(orientation->value[0]) == 'r');
    CHECK(char(orientation->value[1]) == 'd');
    CHECK(tex.find_key("KTXswizzle") == nullptr);
}

// Spreading a read over a scheduler
// -------------------------------------------------------------------------------------------------

TEST("babel::ktx2 - levels decoded over a pool are the levels decoded on the calling thread")
{
    auto pool = cc::async_thread_pool(2);
    auto const levels
        = cc::vector<cc::vector<byte>>{counting(64 * 64 * 4, 1), counting(32 * 32 * 4, 2), counting(16 * 16 * 4, 3)};
    auto const file = make_ktx2({.width = 64, .height = 64, .levels = 3, .scheme = ktx2::supercompression::zstd}, levels);

    auto const plain = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(file)));
    auto const spread = REQUIRED_VALUE(ktx2::read(cc::span<byte const>(file), {.scheduler = &pool}));
    REQUIRE(plain.subresources.size() == spread.subresources.size());
    for (auto i = isize(0); i < plain.subresources.size(); ++i)
        CHECK(same_bytes(plain.subresources[i].bytes.span(), spread.subresources[i].bytes.span())).dump("subresource", i);
}

ASYNC_TEST("babel::ktx2 - read_async resolves to read's result, and fails with read's error")
{
    // No scheduler named, so the read and its levels run on the installed compute scheduler.
    auto const levels = cc::vector<cc::vector<byte>>{counting(64, 9)};
    auto const good = cc::make_pinned_data(make_ktx2({.scheme = ktx2::supercompression::zstd}, levels));
    auto const read = co_await ktx2::read_async(cc::pinned_data<byte const>(good));
    REQUIRE(read.subresources.size() == 1);
    CHECK(same_bytes(read.subresources[0].bytes.span(), levels[0]));

    auto bad = make_ktx2({}, levels);
    bad[0] = byte(0);
    auto const failed = ktx2::read_async(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bad))));
    co_await cc::async_settled(failed);
    CHECK(failed->has_error());
}

// Refusals
// -------------------------------------------------------------------------------------------------

TEST("babel::ktx2 - a malformed file is an error naming what is wrong")
{
    auto const one = cc::vector<cc::vector<byte>>{counting(64, 0)};
    auto const read = [](cc::vector<byte> const& file) { return error_of(ktx2::read(cc::span<byte const>(file))); };

    auto no_id = make_ktx2({}, one);
    no_id[1] = byte('X');
    CHECK(read(no_id).contains("identifier"));

    CHECK(read(cc::vector<byte>::create_filled(40, byte(0))).contains("cannot hold the"));

    CHECK(read(make_ktx2({.scheme = ktx2::supercompression::basis_lz}, one)).contains("Basis Universal"));
    CHECK(read(make_ktx2({.vk_format = 0}, one)).contains("Basis Universal"));
    CHECK(read(make_ktx2({.vk_format = 1000156000}, one)).contains("vkFormat 1000156000"));
    CHECK(read(make_ktx2({.type_size = 4}, one)).contains("typeSize"));
    CHECK(read(make_ktx2({.faces = 3}, one)).contains("faceCount"));
    CHECK(read(make_ktx2({.width = 4, .height = 2, .faces = 6}, one)).contains("cube map must be square"));
    CHECK(read(make_ktx2({.levels = 9}, one)).contains("levelCount 9"));
    CHECK(read(make_ktx2({.vk_format = u32(ktx2::format::bc1_rgba_unorm), .height = 0}, one)).contains("must be 2D"));

    // a level of the wrong size, plain or by its index
    CHECK(read(make_ktx2({}, cc::vector<cc::vector<byte>>{counting(60, 0)})).contains("level 0 stores 60 bytes"));
    CHECK(read(make_ktx2({.scheme = ktx2::supercompression::zstd, .claimed_uncompressed = 32}, one))
              .contains("by its index"));

    // a level past the end of the file
    auto truncated = make_ktx2({}, one);
    truncated.resize_down_to(truncated.size() - 1);
    CHECK(read(truncated).contains("past the end of the file"));

    // supercompressed bytes that do not decode
    auto corrupt = make_ktx2({.scheme = ktx2::supercompression::zstd}, one);
    auto const level_at = isize(cc::load_bytes_le<u64>(corrupt, 80));
    corrupt[level_at] = byte(0);
    CHECK(read(corrupt).contains("does not decode"));
}

TEST("babel::ktx2 - a supercompressed file asking for more than max_decoded_bytes is refused before decoding")
{
    auto const levels = cc::vector<cc::vector<byte>>{counting(64 * 64 * 4, 0)};
    auto const file = make_ktx2({.width = 64, .height = 64, .scheme = ktx2::supercompression::zstd}, levels);
    auto const refused = ktx2::read(cc::span<byte const>(file), {.max_decoded_bytes = 1024});
    CHECK(error_of(refused).contains("1024 bytes"));

    // a plain file's levels are views, so the limit does not apply to it
    auto const plain = make_ktx2({.width = 64, .height = 64}, levels);
    CHECK(ktx2::read(cc::span<byte const>(plain), {.max_decoded_bytes = 1024}).has_value());
}

// Formats
// -------------------------------------------------------------------------------------------------

TEST("babel::ktx2 - image sizes count whole blocks for a compressed format and texels otherwise")
{
    CHECK(ktx2::image_byte_size(ktx2::format::bc1_rgba_unorm, 1, 1, 1) == 8);
    CHECK(ktx2::image_byte_size(ktx2::format::bc7_unorm, 5, 4, 1) == 32);
    CHECK(ktx2::image_byte_size(ktx2::format::r16g16b16a16_sfloat, 3, 2, 2) == 3 * 2 * 2 * 8);
    CHECK(ktx2::is_float(ktx2::format::bc6h_ufloat));
    CHECK(!ktx2::is_float(ktx2::format::bc7_unorm));
    CHECK(ktx2::is_known(u32(ktx2::format::e5b9g9r9_ufloat_pack32)));
    CHECK(!ktx2::is_known(1));
    CHECK(ktx2::to_string(ktx2::format::bc5_snorm) == "bc5_snorm");
}
