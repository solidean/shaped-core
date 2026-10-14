#include <babel-serializer/image/dds.hh>
#include <babel-serializer/image/image.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh>

using namespace cc::primitive_defines;

namespace
{
namespace dds = babel::dds;

constexpr u32 four_cc(char a, char b, char c, char d)
{
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

/// Everything a test varies in a DDS header; the defaults are a 4x4 DXT1 with its base level only.
struct dds_spec
{
    u32 width = 4;
    u32 height = 4;
    u32 depth = 0;
    u32 mips = 0;
    u32 flags = 0x1007;
    u32 pf_flags = 0x4;
    u32 code = four_cc('D', 'X', 'T', '1');
    u32 bit_count = 0;
    u32 r_mask = 0;
    u32 g_mask = 0;
    u32 b_mask = 0;
    u32 a_mask = 0;
    u32 caps2 = 0;

    bool dx10 = false;
    u32 dxgi = 0;
    u32 resource_dimension = 3;
    u32 misc_flag = 0;
    u32 array_size = 1;
    u32 misc_flags2 = 0;
};

/// A DDS file: the header `spec` describes, then `payload` as it is.
cc::vector<byte> make_dds(dds_spec const& spec, cc::span<byte const> payload)
{
    auto const header_size = spec.dx10 ? isize(148) : isize(128);
    auto out = cc::vector<byte>::create_filled(header_size + payload.size(), byte(0));
    cc::store_bytes_le<u32>(out, 0, four_cc('D', 'D', 'S', ' '));
    cc::store_bytes_le<u32>(out, 4, 124);
    cc::store_bytes_le<u32>(out, 8, spec.flags);
    cc::store_bytes_le<u32>(out, 12, spec.height);
    cc::store_bytes_le<u32>(out, 16, spec.width);
    cc::store_bytes_le<u32>(out, 24, spec.depth);
    cc::store_bytes_le<u32>(out, 28, spec.mips);
    cc::store_bytes_le<u32>(out, 76, 32);
    cc::store_bytes_le<u32>(out, 80, spec.dx10 ? 0x4u : spec.pf_flags);
    cc::store_bytes_le<u32>(out, 84, spec.dx10 ? four_cc('D', 'X', '1', '0') : spec.code);
    cc::store_bytes_le<u32>(out, 88, spec.bit_count);
    cc::store_bytes_le<u32>(out, 92, spec.r_mask);
    cc::store_bytes_le<u32>(out, 96, spec.g_mask);
    cc::store_bytes_le<u32>(out, 100, spec.b_mask);
    cc::store_bytes_le<u32>(out, 104, spec.a_mask);
    cc::store_bytes_le<u32>(out, 108, 0x1000);
    cc::store_bytes_le<u32>(out, 112, spec.caps2);
    if (spec.dx10)
    {
        cc::store_bytes_le<u32>(out, 128, spec.dxgi);
        cc::store_bytes_le<u32>(out, 132, spec.resource_dimension);
        cc::store_bytes_le<u32>(out, 136, spec.misc_flag);
        cc::store_bytes_le<u32>(out, 140, spec.array_size);
        cc::store_bytes_le<u32>(out, 144, spec.misc_flags2);
    }
    for (auto i = isize(0); i < payload.size(); ++i)
        out[header_size + i] = payload[i];
    return out;
}

/// `size` bytes, byte i holding i — so a subresource's first byte names its offset into the payload.
cc::vector<byte> counting_payload(isize size)
{
    auto out = cc::vector<byte>::create_filled(size, byte(0));
    for (auto i = isize(0); i < size; ++i)
        out[i] = byte(u8(i & 0xFF));
    return out;
}

/// One BC1 block of a single 565 color, all indices 0.
void put_solid_bc1(cc::vector<byte>& out, u16 color)
{
    auto block = cc::vector<byte>::create_filled(8, byte(0));
    cc::store_bytes_le<u16>(block, 0, color);
    cc::store_bytes_le<u16>(block, 2, color);
    out.push_back_range(block);
}
} // namespace

TEST("dds - a legacy DXT1 reads its header and one subresource")
{
    auto payload = cc::vector<byte>();
    put_solid_bc1(payload, 0xF800);
    auto const file = make_dds({}, payload);

    auto const tex = dds::read(cc::span<byte const>(file));
    REQUIRE(tex.has_value());
    auto const& t = tex.value();
    CHECK(t.format == dds::pixel_format::bc1_unorm);
    CHECK(t.alpha == dds::alpha_mode::unknown);
    CHECK(t.shape == dds::dimension::texture_2d);
    CHECK(t.width == 4);
    CHECK(t.height == 4);
    CHECK(t.mip_count == 1);
    CHECK(t.file_header.pixels.four_cc == four_cc('D', 'X', 'T', '1'));
    CHECK(!t.file_header.has_dx10);
    REQUIRE(t.subresources.size() == 1);
    CHECK(t.subresources[0].bytes.size() == 8);

    auto const texels = dds::decode_rgba8(t, t.subresources[0]);
    REQUIRE(texels.has_value());
    CHECK(texels.value().size() == 4 * 4 * 4);
    CHECK(u8(texels.value()[0]) == 255);
    CHECK(u8(texels.value()[1]) == 0);
    CHECK(u8(texels.value()[3]) == 255);
}

TEST("dds - the legacy FourCCs resolve to one format each")
{
    struct expectation
    {
        u32 code = 0;
        dds::pixel_format format = dds::pixel_format::bc1_unorm;
        dds::alpha_mode alpha = dds::alpha_mode::unknown;
        isize block_bytes = 8;
    };
    expectation const cases[] = {
        {four_cc('D', 'X', 'T', '1'), dds::pixel_format::bc1_unorm, dds::alpha_mode::unknown, 8},
        {four_cc('D', 'X', 'T', '2'), dds::pixel_format::bc2_unorm, dds::alpha_mode::premultiplied, 16},
        {four_cc('D', 'X', 'T', '3'), dds::pixel_format::bc2_unorm, dds::alpha_mode::unknown, 16},
        {four_cc('D', 'X', 'T', '4'), dds::pixel_format::bc3_unorm, dds::alpha_mode::premultiplied, 16},
        {four_cc('D', 'X', 'T', '5'), dds::pixel_format::bc3_unorm, dds::alpha_mode::unknown, 16},
        {four_cc('A', 'T', 'I', '1'), dds::pixel_format::bc4_unorm, dds::alpha_mode::unknown, 8},
        {four_cc('B', 'C', '4', 'U'), dds::pixel_format::bc4_unorm, dds::alpha_mode::unknown, 8},
        {four_cc('B', 'C', '4', 'S'), dds::pixel_format::bc4_snorm, dds::alpha_mode::unknown, 8},
        {four_cc('A', 'T', 'I', '2'), dds::pixel_format::bc5_unorm, dds::alpha_mode::unknown, 16},
        {four_cc('B', 'C', '5', 'U'), dds::pixel_format::bc5_unorm, dds::alpha_mode::unknown, 16},
        {four_cc('B', 'C', '5', 'S'), dds::pixel_format::bc5_snorm, dds::alpha_mode::unknown, 16},
        {113, dds::pixel_format::rgba16_float, dds::alpha_mode::unknown, 128},
    };

    for (auto const& c : cases)
    {
        auto const payload = counting_payload(c.block_bytes);
        auto const tex = dds::read(cc::span<byte const>(make_dds({.code = c.code}, payload)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().format == c.format);
        CHECK(tex.value().alpha == c.alpha);
        CHECK(tex.value().subresources[0].bytes.size() == c.block_bytes);
    }

    auto const unknown = dds::read(cc::span<byte const>(make_dds({.code = four_cc('Y', 'U', 'Y', '2')}, {})));
    CHECK(unknown.has_error());
}

TEST("dds - a block-compressed level rounds up to whole blocks")
{
    // 1x1, 2x2 and 3x3 all fill one block, 5x5 takes four.
    struct expectation
    {
        u32 extent = 1;
        u32 code = 0;
        isize bytes = 0;
    };
    expectation const cases[] = {
        {1, four_cc('D', 'X', 'T', '1'), 8},  {2, four_cc('D', 'X', 'T', '1'), 8},
        {3, four_cc('D', 'X', 'T', '1'), 8},  {4, four_cc('D', 'X', 'T', '1'), 8},
        {5, four_cc('D', 'X', 'T', '1'), 32}, {1, four_cc('D', 'X', 'T', '5'), 16},
        {3, four_cc('A', 'T', 'I', '2'), 16}, {9, four_cc('A', 'T', 'I', '2'), 9 * 16},
    };

    for (auto const& c : cases)
    {
        auto const payload = counting_payload(c.bytes);
        auto const file = make_dds({.width = c.extent, .height = c.extent, .code = c.code}, payload);
        auto const tex = dds::read(cc::span<byte const>(file));
        REQUIRE(tex.has_value());
        CHECK(tex.value().subresources[0].bytes.size() == c.bytes);

        // The decode crops the block to the image.
        auto const texels = dds::decode_rgba8(tex.value(), tex.value().subresources[0]);
        REQUIRE(texels.has_value());
        CHECK(texels.value().size() == isize(c.extent) * c.extent * 4);
    }
}

TEST("dds - a non-power-of-two mip chain halves down to 1x1, flooring")
{
    // 5x3: 5x3 -> 2x1 -> 1x1, three levels of DXT1, the first two blocks wide and the others one.
    auto const payload = counting_payload(16 + 8 + 8);
    auto const tex = dds::read(cc::span<byte const>(make_dds({.width = 5, .height = 3, .mips = 3}, payload)));
    REQUIRE(tex.has_value());
    auto const& t = tex.value();
    REQUIRE(t.subresources.size() == 3);
    CHECK(t.subresources[0].width == 5);
    CHECK(t.subresources[0].height == 3);
    CHECK(t.subresources[1].width == 2);
    CHECK(t.subresources[1].height == 1);
    CHECK(t.subresources[2].width == 1);
    CHECK(t.subresources[2].height == 1);
    CHECK(t.subresources[0].bytes.size() == 16);
    CHECK(u8(t.subresources[2].bytes[0]) == 24); // the fourth block of the payload

    // A fourth level does not exist for 5x3.
    auto const too_many = dds::read(cc::span<byte const>(make_dds({.width = 5, .height = 3, .mips = 4}, payload)));
    CHECK(too_many.has_error());

    // Uncompressed: 7x5 rgba8 through DX10, 7x5 -> 3x2 -> 1x1, rows tightly packed.
    auto const rgba = counting_payload((7 * 5 + 3 * 2 + 1) * 4);
    auto const flat
        = dds::read(cc::span<byte const>(make_dds({.width = 7, .height = 5, .mips = 3, .dx10 = true, .dxgi = 28}, rgba)));
    REQUIRE(flat.has_value());
    CHECK(flat.value().subresources[1].bytes.size() == 3 * 2 * 4);
    CHECK(flat.value().subresources[2].bytes.size() == 4);
    CHECK(flat.value().row_pitch(7) == 28);
}

TEST("dds - a truncated or malformed file is an error, never a short read")
{
    // 8x8 DXT1 with its full chain: 32 + 8 + 8 + 8 bytes.
    auto const full = counting_payload(56);
    auto const spec = dds_spec{.width = 8, .height = 8, .mips = 4};
    CHECK(dds::read(cc::span<byte const>(make_dds(spec, full))).has_value());
    CHECK(dds::read(cc::span<byte const>(make_dds(spec, cc::span<byte const>(full).first_n(55)))).has_error());

    // Bytes past the last level are tolerated.
    auto const longer = counting_payload(60);
    CHECK(dds::read(cc::span<byte const>(make_dds(spec, longer))).has_value());

    // Every prefix of a valid file is rejected, the header ones included.
    auto const file = make_dds(spec, full);
    for (auto size = isize(0); size < file.size(); ++size)
        CHECK(dds::read(cc::span<byte const>(file).first_n(size)).has_error());

    SECTION("bad magic and sizes")
    {
        auto broken = make_dds(spec, full);
        broken[0] = byte('X');
        CHECK(dds::read(cc::span<byte const>(broken)).has_error());

        auto wrong_size = make_dds(spec, full);
        cc::store_bytes_le<u32>(wrong_size, 4, 100);
        CHECK(dds::read(cc::span<byte const>(wrong_size)).has_error());
    }

    SECTION("zero extents and implausible ones")
    {
        CHECK(dds::read(cc::span<byte const>(make_dds({.width = 0}, full))).has_error());
        CHECK(dds::read(cc::span<byte const>(make_dds({.height = 0}, full))).has_error());
        CHECK(dds::read(cc::span<byte const>(make_dds({.width = 1u << 20, .height = 1u << 20}, full))).has_error());
    }

    SECTION("a DX10 header the file is too short for")
    {
        auto const dx10 = make_dds({.dx10 = true, .dxgi = 71}, {});
        CHECK(dds::read(cc::span<byte const>(dx10).first_n(140)).has_error());
        CHECK(dds::read(cc::span<byte const>(dx10)).has_error()); // header complete, block missing
    }

    SECTION("DX10 values the format does not define")
    {
        auto const block = counting_payload(8);
        CHECK(dds::read(cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 9999}, block))).has_error());
        CHECK(dds::read(cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 71, .resource_dimension = 7}, block)))
                  .has_error());
        CHECK(dds::read(cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 71, .array_size = 0}, block))).has_error());
    }

    SECTION("a mip count past an int is still too many")
    {
        // Stored as a u32, these would turn negative as an int and skip every level.
        auto const block = counting_payload(8);
        CHECK(dds::read(cc::span<byte const>(make_dds({.mips = 0xFFFFFFFF}, block))).has_error());
        CHECK(dds::read(cc::span<byte const>(make_dds({.mips = 0x80000000}, block))).has_error());
    }
}

TEST("dds - a cube map stores each face's chain in turn")
{
    // A legacy 4x4 DXT1 cube with two levels: per face 8 bytes of level 0 and 8 of level 1.
    auto const payload = counting_payload(6 * 2 * 8);
    auto const tex = dds::read(cc::span<byte const>(make_dds({.mips = 2, .caps2 = 0x200 | 0xFC00}, payload)));
    REQUIRE(tex.has_value());
    auto const& t = tex.value();
    CHECK(t.is_cube);
    CHECK(t.face_count() == 6);
    REQUIRE(t.subresources.size() == 12);

    for (auto face = 0; face < 6; ++face)
        for (auto mip = 0; mip < 2; ++mip)
        {
            auto const* const s = t.find(0, face, mip);
            REQUIRE(s != nullptr);
            CHECK(u8(s->bytes[0]) == (face * 2 + mip) * 8);
            CHECK(s->width == (mip == 0 ? 4 : 2));
        }

    SECTION("a legacy cube may store only some faces")
    {
        // +X and -Z alone: bits 0x400 and 0x8000.
        auto const partial
            = dds::read(cc::span<byte const>(make_dds({.mips = 1, .caps2 = 0x200 | 0x400 | 0x8000}, payload)));
        REQUIRE(partial.has_value());
        CHECK(partial.value().face_count() == 2);
        CHECK(partial.value().find(0, 0, 0) != nullptr);
        CHECK(partial.value().find(0, 1, 0) == nullptr);
        REQUIRE(partial.value().find(0, 5, 0) != nullptr);
        CHECK(u8(partial.value().find(0, 5, 0)->bytes[0]) == 8);
    }

    SECTION("a DX10 cube array counts cubes, with six faces each")
    {
        auto const cubes = counting_payload(2 * 6 * 16);
        auto const arr = dds::read(
            cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 98, .misc_flag = 0x4, .array_size = 2}, cubes)));
        REQUIRE(arr.has_value());
        CHECK(arr.value().format == dds::pixel_format::bc7_unorm);
        CHECK(arr.value().array_size == 2);
        CHECK(arr.value().subresources.size() == 12);
        REQUIRE(arr.value().find(1, 3, 0) != nullptr);
        CHECK(u8(arr.value().find(1, 3, 0)->bytes[0]) == u8(((6 + 3) * 16) & 0xFF));
    }

    SECTION("a cube map's faces must be square")
    {
        CHECK(dds::read(cc::span<byte const>(make_dds({.width = 8, .caps2 = 0x200 | 0xFC00}, payload))).has_error());
    }
}

TEST("dds - a volume stores each level's slices together")
{
    // 4x4x4 rgba8 through DX10, two levels: 4x4x4 then 2x2x2.
    auto const payload = counting_payload((64 + 8) * 4);
    auto const tex = dds::read(cc::span<byte const>(
        make_dds({.depth = 4, .mips = 2, .dx10 = true, .dxgi = 28, .resource_dimension = 4}, payload)));
    REQUIRE(tex.has_value());
    auto const& t = tex.value();
    CHECK(t.shape == dds::dimension::texture_3d);
    CHECK(t.depth == 4);
    REQUIRE(t.subresources.size() == 2);
    CHECK(t.subresources[1].depth == 2);
    CHECK(t.subresources[1].bytes.size() == 2 * 2 * 2 * 4);

    auto const texels = dds::decode_rgba8(t, t.subresources[1]);
    REQUIRE(texels.has_value());
    CHECK(texels.value().size() == 8 * 4);
}

TEST("dds - a legacy volume is decided by DDSD_DEPTH, as DirectXTex reads it")
{
    // 4x4x4 DXT1, two levels: four blocks, then the 2x2x2 level's two.
    auto const payload = counting_payload((4 + 2) * 8);
    constexpr u32 flag_depth = 0x800000;
    constexpr u32 caps2_volume = 0x200000;

    SECTION("DDSD_DEPTH alone is a volume")
    {
        auto const tex = dds::read(
            cc::span<byte const>(make_dds({.depth = 4, .mips = 2, .flags = 0x1007 | 0x20000 | flag_depth}, payload)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().shape == dds::dimension::texture_3d);
        CHECK(tex.value().depth == 4);
        REQUIRE(tex.value().subresources.size() == 2);
        CHECK(u8(tex.value().subresources[1].bytes[0]) == 4 * 8); // mip 1 starts after mip 0's four slices
    }

    SECTION("DDSCAPS2_VOLUME alone is a 2D texture")
    {
        auto const tex = dds::read(cc::span<byte const>(
            make_dds({.depth = 4, .mips = 2, .flags = 0x1007 | 0x20000, .caps2 = caps2_volume}, payload)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().shape == dds::dimension::texture_2d);
        CHECK(tex.value().depth == 1);
    }
}

TEST("dds - BC5 decodes x to red and y to green")
{
    // Two channel blocks of constant value: red 50, green 200.
    auto payload = cc::vector<byte>::create_filled(16, byte(0));
    payload[0] = byte(50);
    payload[1] = byte(50);
    payload[8] = byte(200);
    payload[9] = byte(200);
    auto const file = make_dds({.code = four_cc('A', 'T', 'I', '2')}, payload);

    auto const tex = dds::read(cc::span<byte const>(file));
    REQUIRE(tex.has_value());
    auto const texels = dds::decode_rgba8(tex.value(), tex.value().subresources[0]);
    REQUIRE(texels.has_value());
    CHECK(u8(texels.value()[0]) == 50);
    CHECK(u8(texels.value()[1]) == 200);
    CHECK(u8(texels.value()[2]) == 0);
    CHECK(u8(texels.value()[3]) == 255);

    // The aggregator keeps all four channels, so x and y stay where a GPU would put them.
    auto const img = babel::image::read(file);
    REQUIRE(img.has_value());
    CHECK(img.value().channels == 4);
    CHECK(img.value().comp == babel::image::component::u8);
    CHECK(u8(img.value().pixels[1]) == 200);
}

TEST("dds - a signed BC5 decodes to -1..1 as floats")
{
    auto payload = cc::vector<byte>::create_filled(16, byte(0));
    payload[0] = byte(0x81); // -127
    payload[1] = byte(0x81);
    payload[8] = byte(0x7F); // 127
    payload[9] = byte(0x7F);
    auto const tex = dds::read(cc::span<byte const>(make_dds({.code = four_cc('B', 'C', '5', 'S')}, payload)));
    REQUIRE(tex.has_value());
    auto const values = dds::decode_rgba32f(tex.value(), tex.value().subresources[0]);
    REQUIRE(values.has_value());
    CHECK(values.value()[0] == -1.0f);
    CHECK(values.value()[1] == 1.0f);
    CHECK(values.value()[2] == 0.0f);
    CHECK(values.value()[3] == 1.0f);
}

TEST("dds - legacy masked layouts")
{
    SECTION("A8R8G8B8 is bgra8 in memory")
    {
        byte const texel[4] = {byte(10), byte(20), byte(30), byte(40)}; // b g r a
        auto const tex = dds::read(cc::span<byte const>(make_dds({.width = 1,
                                                                  .height = 1,
                                                                  .pf_flags = 0x41,
                                                                  .code = 0,
                                                                  .bit_count = 32,
                                                                  .r_mask = 0x00FF0000,
                                                                  .g_mask = 0x0000FF00,
                                                                  .b_mask = 0x000000FF,
                                                                  .a_mask = 0xFF000000},
                                                                 texel)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().format == dds::pixel_format::bgra8_unorm);
        auto const out = dds::decode_rgba8(tex.value(), tex.value().subresources[0]).value();
        CHECK(u8(out[0]) == 30);
        CHECK(u8(out[1]) == 20);
        CHECK(u8(out[2]) == 10);
        CHECK(u8(out[3]) == 40);
    }

    SECTION("an alpha mask without an alpha flag is ignored")
    {
        byte const texel[4] = {byte(10), byte(20), byte(30), byte(40)};
        auto const tex = dds::read(cc::span<byte const>(make_dds({.width = 1,
                                                                  .height = 1,
                                                                  .pf_flags = 0x40,
                                                                  .code = 0,
                                                                  .bit_count = 32,
                                                                  .r_mask = 0x00FF0000,
                                                                  .g_mask = 0x0000FF00,
                                                                  .b_mask = 0x000000FF,
                                                                  .a_mask = 0xFF000000},
                                                                 texel)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().format == dds::pixel_format::bgrx8_unorm);
        CHECK(u8(dds::decode_rgba8(tex.value(), tex.value().subresources[0]).value()[3]) == 255);
    }

    SECTION("R5G6B5 widens each mask to 8 bits")
    {
        auto texel = cc::vector<byte>::create_filled(2, byte(0));
        cc::store_bytes_le<u16>(texel, 0, u16((31u << 11) | (0u << 5) | 16u));
        auto const tex = dds::read(cc::span<byte const>(make_dds({.width = 1,
                                                                  .height = 1,
                                                                  .pf_flags = 0x40,
                                                                  .code = 0,
                                                                  .bit_count = 16,
                                                                  .r_mask = 0xF800,
                                                                  .g_mask = 0x07E0,
                                                                  .b_mask = 0x001F},
                                                                 texel)));
        REQUIRE(tex.has_value());
        CHECK(tex.value().format == dds::pixel_format::masked);
        auto const out = dds::decode_rgba8(tex.value(), tex.value().subresources[0]).value();
        CHECK(u8(out[0]) == 255);
        CHECK(u8(out[1]) == 0);
        CHECK(u8(out[2]) == 132); // 16 / 31 * 255 = 131.6
        CHECK(u8(out[3]) == 255);
    }

    SECTION("L8 replicates into r, g and b, and a 24-bit row is not padded")
    {
        byte const texels[3] = {byte(7), byte(8), byte(9)};
        auto const tex = dds::read(cc::span<byte const>(make_dds(
            {.width = 3, .height = 1, .pf_flags = 0x20000, .code = 0, .bit_count = 8, .r_mask = 0xFF}, texels)));
        REQUIRE(tex.has_value());
        auto const out = dds::decode_rgba8(tex.value(), tex.value().subresources[0]).value();
        CHECK(u8(out[4]) == 8);
        CHECK(u8(out[5]) == 8);
        CHECK(u8(out[6]) == 8);
        CHECK(u8(out[7]) == 255);

        // B8G8R8, 24 bits: 3x1 is 9 bytes with no row padding.
        auto const rgb = counting_payload(9);
        auto const t24 = dds::read(cc::span<byte const>(make_dds({.width = 3,
                                                                  .height = 1,
                                                                  .pf_flags = 0x40,
                                                                  .code = 0,
                                                                  .bit_count = 24,
                                                                  .r_mask = 0xFF0000,
                                                                  .g_mask = 0x00FF00,
                                                                  .b_mask = 0x0000FF},
                                                                 rgb)));
        REQUIRE(t24.has_value());
        CHECK(t24.value().subresources[0].bytes.size() == 9);
        auto const out24 = dds::decode_rgba8(t24.value(), t24.value().subresources[0]).value();
        CHECK(u8(out24[8]) == 8); // texel 2's red is byte 8
    }

    SECTION("a mask that is not one run of bits is an error")
    {
        auto const texel = counting_payload(2);
        CHECK(dds::read(
                  cc::span<byte const>(make_dds(
                      {.width = 1, .height = 1, .pf_flags = 0x40, .code = 0, .bit_count = 16, .r_mask = 0x0F0F}, texel)))
                  .has_error());
    }

    SECTION("a mask past the pixel is an error")
    {
        byte const texel[1] = {byte(0)};
        CHECK(dds::read(
                  cc::span<byte const>(make_dds(
                      {.width = 1, .height = 1, .pf_flags = 0x40, .code = 0, .bit_count = 8, .r_mask = 0xF00}, texel)))
                  .has_error());
    }
}

TEST("dds - DX10 formats and alpha mode")
{
    auto const block = counting_payload(16);
    auto const tex = dds::read(cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 99, .misc_flags2 = 2}, block)));
    REQUIRE(tex.has_value());
    CHECK(tex.value().format == dds::pixel_format::bc7_unorm_srgb);
    CHECK(dds::is_srgb(tex.value().format));
    CHECK(tex.value().alpha == dds::alpha_mode::premultiplied);
    CHECK(tex.value().file_header.has_dx10);

    // A float format refuses the 8-bit decode and takes the float one.
    auto const halves = cc::vector<byte>::create_filled(4 * 4 * 8, byte(0));
    auto const hf = dds::read(cc::span<byte const>(make_dds({.dx10 = true, .dxgi = 10}, halves)));
    REQUIRE(hf.has_value());
    CHECK(dds::decode_rgba8(hf.value(), hf.value().subresources[0]).has_error());
    auto const floats = dds::decode_rgba32f(hf.value(), hf.value().subresources[0]);
    REQUIRE(floats.has_value());
    CHECK(floats.value().size() == 64);

    // The aggregator hands a float format back as f32.
    auto const img = babel::image::read(make_dds({.dx10 = true, .dxgi = 10}, halves));
    REQUIRE(img.has_value());
    CHECK(img.value().comp == babel::image::component::f32);
    CHECK(img.value().channels == 4);
}

TEST("dds - the aggregator detects DDS and keeps mip 0 of the first layer")
{
    // 8x8 DXT1, two levels: level 0 solid red, level 1 solid blue.
    auto payload = cc::vector<byte>();
    for (auto i = 0; i < 4; ++i)
        put_solid_bc1(payload, 0xF800);
    put_solid_bc1(payload, 0x001F);
    auto const file = make_dds({.width = 8, .height = 8, .mips = 2}, payload);

    auto const fmt = babel::image::detect_format(file);
    REQUIRE(fmt.has_value());
    CHECK(fmt.value() == babel::image::format::dds);

    auto const img = babel::image::read(file);
    REQUIRE(img.has_value());
    CHECK(img.value().width == 8);
    CHECK(img.value().height == 8);
    CHECK(img.value().channels == 4);
    CHECK(u8(img.value().pixels[0]) == 255);
    CHECK(u8(img.value().pixels[2]) == 0);

    CHECK(babel::image::encode(img.value(), babel::image::format::dds).has_error());

    // The stream overload pins the slurped buffer for DDS, and must decode the same image.
    auto adapter = cc::span_read_stream_adapter(file);
    cc::read_stream stream = adapter;
    auto const streamed = babel::image::read(stream);
    REQUIRE(streamed.has_value());
    CHECK(streamed.value().width == 8);
    CHECK(u8(streamed.value().pixels[0]) == 255);
}

// Real textures
// -------------------------------------------------------------------------------------------------

namespace
{
cc::optional<cc::vector<byte>> read_file(cc::string_view path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    if (file.has_error())
        return {};
    auto stream = file.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return {};
    return cc::move(bytes.value());
}
} // namespace

TEST("dds - real Bistro textures decode, when present")
{
    // Where extern/bistro/fetch-bistro.py installs them, beside the FBX files the fbx tests read.
    auto const dir = cc::string_view(BABEL_TEST_BISTRO_DIR "/Textures");

    // The small files cover every encoding Bistro uses; the 2048 x 2048 ones run under --thorough only.
    struct sample
    {
        cc::string_view name;
        babel::image::format format = babel::image::format::dds;
        int width = 0;
        int channels = 4;
        bool heavy = false;
    };
    sample const samples[] = {
        {"Antenna_Metal_Specular.dds", babel::image::format::dds, 16, 4},               // DXT5, 16x16
        {"Emissive_StreetLight_Emissive.dds", babel::image::format::dds, 64, 4},        // DXT1, 64x64
        {"Bistro_Sign_Letters_Normal.dds", babel::image::format::dds, 1, 4},            // ATI2, 1x1, no mip count
        {"Emissive_StreetLight_Emissive.tga", babel::image::format::tga, 16, 4},        // 32-bit, footer
        {"M_StatueGlass_Normal.TGA", babel::image::format::tga, 1, 3},                  // 24-bit, 1x1
        {"Antenna_Metal_BaseColor.dds", babel::image::format::dds, 2048, 4, true},      // DXT1, 12 levels
        {"Foliage_Ivy_leaf_a_BaseColor.dds", babel::image::format::dds, 2048, 4, true}, // DXT5
        {"Antenna_Metal_Normal.dds", babel::image::format::dds, 2048, 4, true},         // ATI2
        {"M_StatueGlass_BaseColor.TGA", babel::image::format::tga, 2048, 3, true},      // 24-bit, 12 MB
    };

    auto found = 0;
    for (auto const& s : samples)
    {
        if (s.heavy && !nx::is_thorough())
            continue;
        auto const bytes = read_file(cc::format("{}/{}", dir, s.name));
        if (!bytes.has_value())
            continue;
        ++found;

        auto const fmt = babel::image::detect_format(bytes.value());
        REQUIRE(fmt.has_value());
        CHECK(fmt.value() == s.format);

        auto const img = babel::image::read(bytes.value());
        REQUIRE(img.has_value());
        CHECK(img.value().width == s.width);
        CHECK(img.value().height == s.width);
        CHECK(img.value().channels == s.channels);
        CHECK(img.value().comp == babel::image::component::u8);

        if (s.format == babel::image::format::dds)
        {
            // Every level of the chain is in the file, down to 1x1.
            auto const tex = dds::read(cc::span<byte const>(bytes.value()));
            REQUIRE(tex.has_value());
            auto const& last = tex.value().subresources.back();
            CHECK(last.width == 1);
            CHECK(last.height == 1);
        }
    }

    if (found == 0)
        SKIP("Bistro is not fetched; uv run extern/bistro/fetch-bistro.py installs it");
}

// Budgets and hand-built input
// -------------------------------------------------------------------------------------------------

TEST("dds - a decode larger than the budget is refused before allocating")
{
    auto const payload = counting_payload(4 * 8); // 8x8 DXT1
    auto const tex = dds::read(cc::span<byte const>(make_dds({.width = 8, .height = 8}, payload)));
    REQUIRE(tex.has_value());
    auto const& sub = tex.value().subresources[0];

    // 8 x 8 rgba8 is 256 bytes, and rgba32f four times that.
    CHECK(dds::decode_rgba8(tex.value(), sub, {.max_decoded_bytes = 256}).has_value());
    CHECK(dds::decode_rgba8(tex.value(), sub, {.max_decoded_bytes = 255}).has_error());
    CHECK(dds::decode_rgba32f(tex.value(), sub, {.max_decoded_bytes = 1023}).has_error());

    auto const file = make_dds({.width = 8, .height = 8}, payload);
    CHECK(babel::image::read(file, {.max_decoded_bytes = 255}).has_error());
}

TEST("dds - a float DDS reaches the aggregator as f32, one buffer under the budget")
{
    // 2x2 rgba32_float through DX10: texel t holds (t, t + 0.25, t + 0.5, 1).
    auto payload = cc::vector<byte>::create_filled(2 * 2 * 16, byte(0));
    for (auto t = 0; t < 4; ++t)
        for (auto c = 0; c < 4; ++c)
            cc::store_bytes_le<float>(payload, (t * 4 + c) * 4, c == 3 ? 1.0f : float(t) + 0.25f * float(c));
    auto const file = make_dds({.width = 2, .height = 2, .dx10 = true, .dxgi = 2}, payload);

    auto const img = babel::image::read(file);
    REQUIRE(img.has_value());
    CHECK(img.value().comp == babel::image::component::f32);
    auto const floats = img.value().samples_f32();
    REQUIRE(floats.size() == 16);
    CHECK(floats[2 * 4 + 1] == 2.25f);
    CHECK(floats[3 * 4 + 3] == 1.0f);

    // 64 bytes of floats: the budget is the image's own size, with nothing beside it.
    CHECK(babel::image::read(file, {.max_decoded_bytes = 64}).has_value());
    CHECK(babel::image::read(file, {.max_decoded_bytes = 63}).has_error());
}

TEST("dds - decoding checks a hand-built texture rather than trusting it")
{
    auto tex = dds::data{.format = dds::pixel_format::masked, .width = 1, .height = 1};
    tex.file_header.pixels = {.flags = 0x40, .rgb_bit_count = 40, .r_mask = 0xFF};
    auto const bytes = counting_payload(8);
    auto const sub = dds::subresource{.width = 1, .height = 1, .bytes = cc::make_pinned_data(bytes)};
    CHECK(dds::decode_rgba8(tex, sub).has_error());
    CHECK(dds::decode_rgba32f(tex, sub).has_error());

    // An extent no reader produces, whose pitch would otherwise run past an int.
    auto bc = dds::data{.format = dds::pixel_format::bc1_unorm, .width = 1, .height = 1};
    auto const huge = dds::subresource{.width = 2147483647, .height = 1, .bytes = cc::make_pinned_data(bytes)};
    CHECK(dds::decode_rgba8(bc, huge).has_error());
}

TEST("dds - the float decode of BC1 keeps its exact interpolants")
{
    // A 4x4 DXT1 with r = 1 (8 widened) over black; texel 2 takes index 2, two thirds of the way: 16/3.
    auto payload = cc::vector<byte>::create_filled(8, byte(0));
    cc::store_bytes_le<u16>(payload, 0, 0x0800);
    cc::store_bytes_le<u32>(payload, 4, 0x20);
    auto const tex = dds::read(cc::span<byte const>(make_dds({}, payload)));
    REQUIRE(tex.has_value());
    auto const floats = dds::decode_rgba32f(tex.value(), tex.value().subresources[0]);
    REQUIRE(floats.has_value());
    CHECK(floats.value()[2 * 4 + 0] == 16.0f / 765.0f);
    auto const bytes = dds::decode_rgba8(tex.value(), tex.value().subresources[0]);
    REQUIRE(bytes.has_value());
    CHECK(u8(bytes.value()[2 * 4 + 0]) == 5);
}
