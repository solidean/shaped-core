#include <babel-serializer/image/bcn.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Every expectation here is computed by hand from the decoding rules stated in bcn.hh, never by running a decoder.
// The rounding cases are chosen so that truncating instead of rounding would land one below.

namespace
{
namespace bcn = babel::bcn;

using block8 = cc::fixed_array<byte, 8>;
using block16 = cc::fixed_array<byte, 16>;

/// A BC1-style color block: two RGB565 endpoints and sixteen 2-bit indices, texel 0 in the lowest bits.
block8 make_color_block(u16 c0, u16 c1, cc::span<int const> indices)
{
    auto out = block8{};
    auto packed = u32(0);
    for (auto i = 0; i < 16; ++i)
        packed |= u32(indices[i]) << (2 * i);
    cc::store_bytes_le<u16>(out, 0, c0);
    cc::store_bytes_le<u16>(out, 2, c1);
    cc::store_bytes_le<u32>(out, 4, packed);
    return out;
}

/// A BC4-style channel block: two endpoints and sixteen 3-bit indices.
block8 make_channel_block(u8 e0, u8 e1, cc::span<int const> indices)
{
    auto packed = u64(0);
    for (auto i = 0; i < 16; ++i)
        packed |= u64(indices[i]) << (3 * i);
    auto out = block8{};
    cc::store_bytes_le<u64>(out, 0, (packed << 16) | (u64(e1) << 8) | u64(e0));
    return out;
}

block16 concat(block8 const& lo, block8 const& hi)
{
    auto out = block16{};
    for (auto i = 0; i < 8; ++i)
    {
        out[i] = lo[i];
        out[i + 8] = hi[i];
    }
    return out;
}

/// Writes fields least significant bit first, the order BC6H and BC7 read them in.
struct bit_writer
{
    block16 bytes = {};
    int pos = 0;

    void put(u32 value, int count)
    {
        for (auto b = 0; b < count; ++b)
        {
            if (((value >> b) & 1u) != 0)
                bytes[(pos + b) / 8] = byte(u8(bytes[(pos + b) / 8]) | u8(1u << ((pos + b) % 8)));
        }
        pos += count;
    }
};

constexpr auto texel_order = cc::fixed_array<int, 16>{0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
constexpr auto channel_order = cc::fixed_array<int, 16>{0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};

bool texel_is(bcn::rgba8_block const& b, int texel, int r, int g, int bl, int a)
{
    return b.rgba[texel * 4 + 0] == r && b.rgba[texel * 4 + 1] == g && b.rgba[texel * 4 + 2] == bl
        && b.rgba[texel * 4 + 3] == a;
}
// The decoders take fixed-extent spans; these wrap the test's own block arrays, temporaries included.
bcn::rgba8_block decode_bc1(block8 const& b)
{
    return bcn::decode_bc1(bcn::small_block(b));
}
bcn::rgba8_block decode_bc2(block16 const& b)
{
    return bcn::decode_bc2(bcn::large_block(b));
}
bcn::rgba8_block decode_bc3(block16 const& b)
{
    return bcn::decode_bc3(bcn::large_block(b));
}
bcn::rgba8_block decode_bc4(block8 const& b, bool is_signed)
{
    return bcn::decode_bc4(bcn::small_block(b), is_signed);
}
bcn::rgba8_block decode_bc5(block16 const& b, bool is_signed)
{
    return bcn::decode_bc5(bcn::large_block(b), is_signed);
}
bcn::rgba8_block decode_bc7(block16 const& b)
{
    return bcn::decode_bc7(bcn::large_block(b));
}
bcn::rgba32f_block decode_bc6h(block16 const& b, bool is_signed)
{
    return bcn::decode_bc6h(bcn::large_block(b), is_signed);
}
cc::fixed_array<i16, 16> decode_bc4_channel(block8 const& b, bool is_signed)
{
    return bcn::decode_bc4_channel(bcn::small_block(b), is_signed);
}
} // namespace

TEST("bcn - BC1 four-color mode interpolates thirds, rounded to nearest")
{
    // Pure red over pure blue: c0 > c1 selects the four-color mode.
    auto const block = make_color_block(0xF800, 0x001F, texel_order);
    auto const out = decode_bc1(block);

    CHECK(texel_is(out, 0, 255, 0, 0, 255));
    CHECK(texel_is(out, 1, 0, 0, 255, 255));
    CHECK(texel_is(out, 2, 170, 0, 85, 255)); // (2 * 255 + 0) / 3 = 170, (0 + 255) / 3 = 85
    CHECK(texel_is(out, 3, 85, 0, 170, 255));
    CHECK(texel_is(out, 15, 85, 0, 170, 255));
}

TEST("bcn - BC1 widens 565 by bit replication and rounds the thirds")
{
    // r = 1 widens to 8; two thirds of 8 is 5.33 and one third 2.67, so rounding gives 5 and 3 where truncating
    // would give 5 and 2.
    auto const block = make_color_block(0x0800, 0x0000, texel_order);
    auto const out = decode_bc1(block);

    CHECK(texel_is(out, 0, 8, 0, 0, 255));
    CHECK(texel_is(out, 2, 5, 0, 0, 255));
    CHECK(texel_is(out, 3, 3, 0, 0, 255));

    // The widest values replicate their top bits: 31 -> 255, 63 -> 255, and 16 -> 132.
    auto const white = decode_bc1(make_color_block(0xFFFF, 0x8000, texel_order));
    CHECK(texel_is(white, 0, 255, 255, 255, 255));
    CHECK(texel_is(white, 1, 132, 0, 0, 255));
}

TEST("bcn - BC1 with c0 <= c1 is the three-color mode with transparent black")
{
    // Blue over red: c0 < c1, so index 2 is the midpoint and index 3 transparent black.
    auto const out = decode_bc1(make_color_block(0x001F, 0xF800, texel_order));
    CHECK(texel_is(out, 0, 0, 0, 255, 255));
    CHECK(texel_is(out, 1, 255, 0, 0, 255));
    CHECK(texel_is(out, 2, 128, 0, 128, 255)); // 127.5 rounds up
    CHECK(texel_is(out, 3, 0, 0, 0, 0));

    // Equal endpoints are not "greater", so they select the three-color mode too.
    auto const equal = decode_bc1(make_color_block(0xFFFF, 0xFFFF, texel_order));
    CHECK(texel_is(equal, 2, 255, 255, 255, 255));
    CHECK(texel_is(equal, 3, 0, 0, 0, 0));
}

TEST("bcn - BC2 takes explicit 4-bit alpha and never the three-color mode")
{
    auto alpha = block8{};
    cc::store_bytes_le<u64>(alpha, 0, 0xFEDCBA9876543210ull);

    // c0 < c1 would be BC1's three-color mode; BC2's color half ignores that.
    auto const out = decode_bc2(concat(alpha, make_color_block(0x001F, 0xF800, texel_order)));

    for (auto i = 0; i < 16; ++i)
        CHECK(out.rgba[i * 4 + 3] == i * 17);
    CHECK(texel_is(out, 2, 85, 0, 170, 2 * 17)); // a third of the way from blue to red
    CHECK(texel_is(out, 3, 170, 0, 85, 3 * 17)); // not transparent black
}

TEST("bcn - BC3 alpha has an 8-value and a 6-value mode")
{
    auto const color = make_color_block(0xFFFF, 0x0000, texel_order);

    SECTION("a0 > a1 interpolates six values in sevenths")
    {
        auto const out = decode_bc3(concat(make_channel_block(200, 10, channel_order), color));
        int const expected[8] = {200, 10, 173, 146, 119, 91, 64, 37};
        for (auto i = 0; i < 16; ++i)
            CHECK(out.rgba[i * 4 + 3] == expected[i % 8]);
    }

    SECTION("a0 <= a1 interpolates four values in fifths, then 0 and 255")
    {
        auto const out = decode_bc3(concat(make_channel_block(10, 200, channel_order), color));
        int const expected[8] = {10, 200, 48, 86, 124, 162, 0, 255};
        for (auto i = 0; i < 16; ++i)
            CHECK(out.rgba[i * 4 + 3] == expected[i % 8]);
    }

    SECTION("the color half is four-color regardless of endpoint order")
    {
        auto const out = decode_bc3(concat(make_channel_block(255, 255, channel_order), //
                                           make_color_block(0x001F, 0xF800, texel_order)));
        CHECK(texel_is(out, 3, 170, 0, 85, 255));
    }
}

TEST("bcn - BC4 decodes to red, unsigned and signed")
{
    SECTION("unsigned")
    {
        auto const out = decode_bc4(make_channel_block(200, 10, channel_order), false);
        CHECK(texel_is(out, 2, 173, 0, 0, 255));
    }

    SECTION("signed values span -127..127, and -128 reads as -127")
    {
        // -128 against 127: the stored bytes compare 0x80 as -128, which is not greater, so this is the 6-value mode.
        auto const values = decode_bc4_channel(make_channel_block(0x80, 0x7F, channel_order), true);
        int const expected[8] = {-127, 127, -76, -25, 25, 76, -127, 127};
        for (auto i = 0; i < 16; ++i)
            CHECK(values[i] == expected[i % 8]);
    }

    SECTION("the mode is chosen before -128 is clamped")
    {
        // -127 > -128 as stored selects the 8-value mode, even though both endpoints then read as -127.
        // Index 7 tells the modes apart: the 8-value mode interpolates it to -127, the 6-value mode pins it to 127.
        auto const values = decode_bc4_channel(make_channel_block(0x81, 0x80, channel_order), true);
        CHECK(values[7] == -127);
    }

    SECTION("signed values land in 8 bits biased")
    {
        int const indices[16] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        auto const out = decode_bc4(make_channel_block(0x81, 0x7F, indices), true);
        CHECK(texel_is(out, 0, 0, 0, 0, 255));   // -127
        CHECK(texel_is(out, 1, 255, 0, 0, 255)); // 127

        auto const zero = decode_bc4(make_channel_block(0, 0, indices), true);
        CHECK(texel_is(zero, 0, 128, 0, 0, 255));
    }
}

TEST("bcn - BC5 is red from the first block and green from the second")
{
    int const zeros[16] = {};
    auto const block = concat(make_channel_block(50, 50, zeros), make_channel_block(200, 200, zeros));
    auto const out = decode_bc5(block, false);
    for (auto i = 0; i < 16; ++i)
        CHECK(texel_is(out, i, 50, 200, 0, 255));
}

TEST("bcn - BC7 mode 6 interpolates in 64ths with its p-bits")
{
    // Mode 6: one subset, 7-bit RGBA endpoints plus a p-bit each, 4-bit indices.
    // Endpoint 0 is 127 with p-bit 1 (255), endpoint 1 is 0 with p-bit 0, and texel i takes index i.
    auto w = bit_writer{};
    w.put(1u << 6, 7);
    for (auto c = 0; c < 4; ++c)
    {
        w.put(127, 7);
        w.put(0, 7);
    }
    w.put(1, 1);
    w.put(0, 1);
    for (auto i = 0; i < 16; ++i)
        w.put(u32(i), i == 0 ? 3 : 4);
    CHECK(w.pos == 128);

    auto const out = decode_bc7(w.bytes);
    // (255 * (64 - w) + 32) >> 6 for weights 0, 4, 34 and 64.
    CHECK(texel_is(out, 0, 255, 255, 255, 255));
    CHECK(texel_is(out, 1, 239, 239, 239, 239));
    CHECK(texel_is(out, 8, 120, 120, 120, 120));
    CHECK(texel_is(out, 15, 0, 0, 0, 0));
}

TEST("bcn - BC7 mode 5 rotation swaps alpha back into its channel")
{
    // Mode 5 with rotation 1, so the alpha slot holds red.
    // Color endpoints are 0 and alpha endpoints 255.
    auto w = bit_writer{};
    w.put(1u << 5, 6);
    w.put(1, 2); // rotation
    for (auto c = 0; c < 3; ++c)
    {
        w.put(0, 7);
        w.put(0, 7);
    }
    w.put(255, 8);
    w.put(255, 8);
    for (auto i = 0; i < 16; ++i)
        w.put(0, i == 0 ? 1 : 2);
    for (auto i = 0; i < 16; ++i)
        w.put(0, i == 0 ? 1 : 2);
    CHECK(w.pos == 128);

    auto const out = decode_bc7(w.bytes);
    CHECK(texel_is(out, 0, 255, 0, 0, 0));
}

TEST("bcn - BC7 and BC6H reserved modes decode to zero")
{
    auto const zero = block16{};
    auto const bc7 = decode_bc7(zero);
    for (auto i = 0; i < 64; ++i)
        CHECK(bc7.rgba[i] == 0);

    // 0x13 is one of BC6H's four reserved 5-bit mode codes.
    auto reserved = block16{};
    reserved[0] = byte(0x13);
    auto const bc6 = decode_bc6h(reserved, false);
    for (auto i = 0; i < 64; ++i)
        CHECK(bc6.rgba[i] == 0.0f);
}

TEST("bcn - BC6H mode 11 keeps 10-bit endpoints and interpolates in halves")
{
    // Mode 0x03: one region, untransformed 10-bit endpoints, 4-bit indices.
    // Endpoint 0 is 0 and endpoint 1 the largest 10-bit value, which unquantizes to 0xFFFF.
    auto w = bit_writer{};
    w.put(0x03, 5);
    for (auto c = 0; c < 3; ++c)
        w.put(0, 10);
    for (auto c = 0; c < 3; ++c)
        w.put(1023, 10);
    for (auto i = 0; i < 16; ++i)
        w.put(i == 0 ? 0 : 15, i == 0 ? 3 : 4);
    CHECK(w.pos == 128);

    auto const out = decode_bc6h(w.bytes, false);
    CHECK(out.rgba[0] == 0.0f);
    CHECK(out.rgba[3] == 1.0f);
    // 0xFFFF * 31 >> 6 is 0x7BFF, the largest finite half.
    CHECK(out.rgba[4] == 65504.0f);
}

// Reference blocks
// -------------------------------------------------------------------------------------------------

// The blocks below are random, and their expected texels are Pillow 12.3's decode of them — an independent
// implementation, run once to produce these numbers.
// BC7 is integer-specified, so the match is exact; it covers every mode and the last rows of the three-subset table.
// Pillow hands BC6H back as 8 bits, clamped to 0..1 and truncated, so that comparison allows one step.
// Unsigned BC6H only: Pillow skips the sign extension after a signed block's inverse transform, and babel does not.

namespace
{
struct bc7_reference
{
    int mode = 0;
    cc::fixed_array<u8, 16> block = {};
    cc::fixed_array<u8, 64> rgba = {};
};

struct bc6h_reference
{
    int code = 0;
    cc::fixed_array<u8, 16> block = {};
    cc::fixed_array<u8, 48> rgb = {};
};

constexpr bc7_reference bc7_references[] = {
    {0,
     {0x9B, 0x40, 0x53, 0x32, 0x3D, 0x31, 0xD2, 0xF0, 0x1D, 0x4A, 0x79, 0xF3, 0x8E, 0xFF, 0x9C, 0x3C},
     {0x2A, 0xBA, 0x74, 0xFF, 0xA1, 0x91, 0xF4, 0xFF, 0x52, 0x48, 0x23, 0xFF, 0x84, 0x81, 0x46, 0xFF,
      0x2A, 0xBA, 0x74, 0xFF, 0xA1, 0x91, 0xF4, 0xFF, 0x31, 0x23, 0x0C, 0xFF, 0x84, 0x81, 0x46, 0xFF,
      0x08, 0xEF, 0x8C, 0xFF, 0x9C, 0x8C, 0xEF, 0xFF, 0x31, 0x23, 0x0C, 0xFF, 0x94, 0x94, 0x52, 0xFF,
      0x20, 0xC9, 0x7B, 0xFF, 0xA0, 0x8F, 0xF2, 0xFF, 0x94, 0x94, 0x52, 0xFF, 0x21, 0x10, 0x00, 0xFF}},
    {1,
     {0xA2, 0x8E, 0xA1, 0xA9, 0x80, 0x00, 0xD9, 0x47, 0x07, 0xD5, 0xF5, 0x7F, 0x2C, 0xCA, 0xD0, 0x30},
     {0x36, 0x03, 0x2A, 0xFF, 0xA9, 0xD9, 0xD5, 0xFF, 0x83, 0x81, 0x7F, 0xFF, 0xA9, 0xD9, 0xD5, 0xFF,
      0x1A, 0x0A, 0x76, 0xFF, 0x3A, 0x02, 0x1E, 0xFF, 0x83, 0x81, 0x7F, 0xFF, 0x71, 0x56, 0x55, 0xFF,
      0x7A, 0x6B, 0x6A, 0xFF, 0x71, 0x56, 0x55, 0xFF, 0x2D, 0x05, 0x43, 0xFF, 0x3A, 0x02, 0x1E, 0xFF,
      0x97, 0xAE, 0xAB, 0xFF, 0x71, 0x56, 0x55, 0xFF, 0x8E, 0x98, 0x96, 0xFF, 0x36, 0x03, 0x2A, 0xFF}},
    {2,
     {0xE4, 0xFF, 0xD5, 0x63, 0x2E, 0x54, 0x52, 0x1E, 0x16, 0x8D, 0xC7, 0x42, 0x88, 0x78, 0x34, 0x39},
     {0xE9, 0x3A, 0x34, 0xFF, 0xFF, 0x42, 0x42, 0xFF, 0xD3, 0x31, 0x26, 0xFF, 0x9C, 0x18, 0x08, 0xFF,
      0xFF, 0x42, 0x42, 0xFF, 0xBD, 0x29, 0x18, 0xFF, 0xBD, 0x29, 0x18, 0xFF, 0xD6, 0x4A, 0x7B, 0xFF,
      0xD3, 0x31, 0x26, 0xFF, 0xD3, 0x31, 0x26, 0xFF, 0xE9, 0x3A, 0x34, 0xFF, 0x70, 0x81, 0x08, 0xFF,
      0xFF, 0x42, 0x42, 0xFF, 0xBD, 0x29, 0x18, 0xFF, 0xE9, 0x3A, 0x34, 0xFF, 0xD6, 0x4A, 0x7B, 0xFF}},
    {3,
     {0xF8, 0x13, 0x14, 0xD8, 0x33, 0xB2, 0x8F, 0xE6, 0x70, 0x98, 0xB4, 0x89, 0x13, 0x19, 0xD6, 0xCD},
     {0x08, 0x90, 0x38, 0xFF, 0xC5, 0x6B, 0x3D, 0xFF, 0x08, 0x90, 0x38, 0xFF, 0x11, 0xD8, 0x79, 0xFF,
      0x08, 0x90, 0x38, 0xFF, 0xCF, 0x39, 0x27, 0xFF, 0x08, 0x90, 0x38, 0xFF, 0x08, 0x90, 0x38, 0xFF,
      0x15, 0xFB, 0x99, 0xFF, 0xC5, 0x6B, 0x3D, 0xFF, 0xC5, 0x6B, 0x3D, 0xFF, 0xCF, 0x39, 0x27, 0xFF,
      0x11, 0xD8, 0x79, 0xFF, 0xBB, 0x9F, 0x53, 0xFF, 0xC5, 0x6B, 0x3D, 0xFF, 0xBB, 0x9F, 0x53, 0xFF}},
    {4,
     {0x70, 0xB5, 0xBD, 0x36, 0xBE, 0x54, 0xD9, 0x16, 0x18, 0x93, 0x76, 0xD3, 0x14, 0xEA, 0xC8, 0xC8},
     {0xAD, 0x7B, 0x4E, 0x18, 0x6B, 0x6B, 0x53, 0xFF, 0x81, 0x70, 0x52, 0xB3, 0x97, 0x76, 0x4B, 0x64,
      0x6B, 0x6B, 0x52, 0xFF, 0x81, 0x70, 0x4B, 0xB3, 0xAD, 0x7B, 0x52, 0x18, 0xAD, 0x7B, 0x49, 0x18,
      0xAD, 0x7B, 0x4C, 0x18, 0x6B, 0x6B, 0x52, 0xFF, 0xAD, 0x7B, 0x4E, 0x18, 0x81, 0x70, 0x50, 0xB3,
      0x97, 0x76, 0x50, 0x64, 0x81, 0x70, 0x4B, 0xB3, 0xAD, 0x7B, 0x4C, 0x18, 0x97, 0x76, 0x53, 0x64}},
    {5,
     {0x60, 0xF9, 0x1B, 0x45, 0x71, 0xA0, 0xC6, 0xEB, 0x8D, 0xF9, 0x1F, 0x59, 0xEF, 0x87, 0x0E, 0xD4},
     {0xCA, 0x21, 0x41, 0xC7, 0x7A, 0x21, 0x41, 0xC7, 0xA1, 0x28, 0x0E, 0xF3, 0x7A, 0x14, 0xA9, 0x6E,
      0x7A, 0x28, 0x0E, 0xF3, 0xCA, 0x14, 0xA9, 0x6E, 0xF1, 0x14, 0xA9, 0x6E, 0xA1, 0x14, 0xA9, 0x6E,
      0xA1, 0x14, 0xA9, 0x6E, 0x7A, 0x14, 0xA9, 0x6E, 0xF1, 0x28, 0x0E, 0xF3, 0xF1, 0x1B, 0x76, 0x9A,
      0xF1, 0x28, 0x0E, 0xF3, 0xCA, 0x14, 0xA9, 0x6E, 0xCA, 0x1B, 0x76, 0x9A, 0x7A, 0x1B, 0x76, 0x9A}},
    {6,
     {0xC0, 0xC5, 0xF9, 0x67, 0xC0, 0x95, 0xF2, 0x9B, 0x87, 0x3F, 0x38, 0x92, 0x78, 0x67, 0xBC, 0x23},
     {0x3C, 0x68, 0x69, 0xCD, 0x79, 0x42, 0x5D, 0x8F, 0xCF, 0x0D, 0x4B, 0x37, 0x3C, 0x68, 0x69, 0xCD,
      0x79, 0x42, 0x5D, 0x8F, 0x3C, 0x68, 0x69, 0xCD, 0x31, 0x6F, 0x6C, 0xD9, 0x84, 0x3B, 0x5A, 0x83,
      0x79, 0x42, 0x5D, 0x8F, 0x6D, 0x4A, 0x5F, 0x9B, 0x6D, 0x4A, 0x5F, 0x9B, 0x62, 0x51, 0x62, 0xA7,
      0xAA, 0x24, 0x53, 0x5D, 0x9E, 0x2B, 0x55, 0x69, 0x3C, 0x68, 0x69, 0xCD, 0x31, 0x6F, 0x6C, 0xD9}},
    {7,
     {0x80, 0xE1, 0x62, 0x03, 0xF2, 0xFC, 0x08, 0x0D, 0x7E, 0xA1, 0x0A, 0xBE, 0x5B, 0xC2, 0xFD, 0x6E},
     {0x59, 0xE3, 0xA2, 0x41, 0x65, 0xCF, 0x0C, 0xAE, 0x61, 0xD6, 0x3D, 0x8A, 0x59, 0xE3, 0xA2, 0x41,
      0x3F, 0x60, 0xC4, 0x56, 0x1C, 0x7D, 0xF7, 0x04, 0x63, 0x41, 0x90, 0xAD, 0x86, 0x24, 0x5D, 0xFF,
      0x61, 0xD6, 0x3D, 0x8A, 0x65, 0xCF, 0x0C, 0xAE, 0x65, 0xCF, 0x0C, 0xAE, 0x5D, 0xDC, 0x71, 0x65,
      0x86, 0x24, 0x5D, 0xFF, 0x3F, 0x60, 0xC4, 0x56, 0x86, 0x24, 0x5D, 0xFF, 0x1C, 0x7D, 0xF7, 0x04}},
};

constexpr bc6h_reference bc6h_references[] = {
    {0x00,
     {0x18, 0x0B, 0x2E, 0x1E, 0x2D, 0x0C, 0x0B, 0x01, 0x1E, 0x0F, 0x52, 0xFC, 0xE7, 0x17, 0x07, 0x81},
     {0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00,
      0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00,
      0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF}},
    {0x01,
     {0x1D, 0x17, 0x35, 0x27, 0x2E, 0x30, 0x14, 0x10, 0x21, 0x31, 0x32, 0xB0, 0x83, 0xE2, 0x26, 0x7D},
     {0x6B, 0xFF, 0x00, 0x3B, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0x98, 0xFF, 0xC3, 0xF1,
      0xFF, 0xFF, 0x6B, 0xFF, 0x00, 0x11, 0xFF, 0x00, 0x82, 0xFF, 0x14, 0xB0, 0xFF, 0xFF, 0x98, 0xFF,
      0xC3, 0x98, 0xFF, 0xC3, 0x82, 0xFF, 0x14, 0x82, 0xFF, 0x14, 0xF1, 0xFF, 0xFF, 0x98, 0xFF, 0xC3}},
    {0x0A,
     {0x8A, 0x23, 0x13, 0x0E, 0x05, 0x1F, 0x11, 0x26, 0x1D, 0x15, 0xB5, 0x78, 0x69, 0xAE, 0x95, 0xD2},
     {0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00,
      0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00,
      0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07}},
    {0x1E,
     {0x3E, 0x3C, 0x23, 0x31, 0x1C, 0x08, 0x0E, 0x27, 0x1F, 0x39, 0x5E, 0x18, 0xD7, 0xA5, 0xBC, 0x25},
     {0x08, 0x1B, 0x07, 0xFF, 0x4F, 0x02, 0x01, 0xFF, 0x00, 0xFF, 0xFF, 0x01, 0x96, 0x00, 0x12, 0x00,
      0xFF, 0x01, 0x00, 0xFF, 0x02, 0xFF, 0x0E, 0x03, 0x00, 0xFF, 0x02, 0x01, 0xF3, 0x04, 0x24, 0x03,
      0x0B, 0x00, 0xFF, 0x01, 0x08, 0x1B, 0x07, 0x08, 0x1B, 0x07, 0x96, 0x00, 0x12, 0x96, 0x00, 0x12}},
    {0x03,
     {0x03, 0x22, 0x19, 0x0C, 0x35, 0x1A, 0x3B, 0x07, 0x0C, 0x17, 0x81, 0x17, 0xB9, 0x3C, 0x65, 0x04},
     {0xFF, 0x00, 0x1C, 0x02, 0x00, 0xFF, 0xFF, 0x01, 0x0C, 0x05, 0x00, 0xFF, 0x05, 0x00, 0xFF, 0xFF,
      0x02, 0x05, 0xFF, 0x01, 0x0C, 0x05, 0x00, 0xFF, 0xFF, 0x04, 0x02, 0xFF, 0x0F, 0x00, 0xFF, 0x1B,
      0x00, 0x1B, 0x00, 0xFF, 0x77, 0x00, 0x52, 0xFF, 0x00, 0x1C, 0x39, 0x00, 0xBD, 0x02, 0x00, 0xFF}},
    {0x0B,
     {0x8B, 0x3F, 0x2F, 0x17, 0x03, 0x1C, 0x0B, 0x3C, 0x19, 0x36, 0x4E, 0xEA, 0xA7, 0xE5, 0x5A, 0x5A},
     {0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0E, 0xFF, 0x00, 0x11, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x19, 0xFF,
      0x00, 0x0F, 0xFF, 0x00, 0x15, 0xFF, 0x00, 0x19, 0xFF, 0x00, 0x12, 0xFF, 0x00, 0x15, 0xFF, 0x00,
      0x10, 0xFF, 0x00, 0x19, 0xFF, 0x00, 0x15, 0xFF, 0x00, 0x10, 0xFF, 0x00, 0x15, 0xFF, 0x00, 0x10}},
    {0x0F,
     {0xCF, 0x30, 0x35, 0x2C, 0x2A, 0x1F, 0x37, 0x10, 0x3B, 0x12, 0x31, 0xC0, 0x4A, 0xC8, 0x5C, 0x6A},
     {0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF,
      0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF,
      0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0x02}},
};

block16 as_block(cc::fixed_array<u8, 16> const& bytes)
{
    auto out = block16{};
    for (auto i = 0; i < 16; ++i)
        out[i] = byte(bytes[i]);
    return out;
}
} // namespace

TEST("bcn - BC7 matches an independent decoder in every mode")
{
    for (auto const& ref : bc7_references)
    {
        auto const out = decode_bc7(as_block(ref.block));
        auto matches = true;
        for (auto i = 0; i < 64; ++i)
            matches = matches && out.rgba[i] == ref.rgba[i];
        CHECK(matches);
    }
}

TEST("bcn - unsigned BC6H matches an independent decoder")
{
    for (auto const& ref : bc6h_references)
    {
        auto const out = decode_bc6h(as_block(ref.block), false);
        auto matches = true;
        for (auto t = 0; t < 16; ++t)
            for (auto c = 0; c < 3; ++c)
            {
                auto const v = out.rgba[t * 4 + c];
                auto const mine = v < 0.0f ? 0 : (v > 1.0f ? 255 : int(v * 255.0f));
                auto const diff = mine - int(ref.rgb[t * 3 + c]);
                matches = matches && diff >= -1 && diff <= 1;
            }
        CHECK(matches);
    }
}

TEST("bcn - signed BC6H sign-extends an endpoint after its inverse transform")
{
    // Mode 0x07: one region, 11-bit base endpoints, 9-bit deltas.
    // Base 10 on every channel, delta -20, so the second endpoint is -10 — negative only if it is sign-extended
    // after the transform.
    auto w = bit_writer{};
    w.put(0x07, 5);
    for (auto c = 0; c < 3; ++c)
        w.put(10, 10);
    for (auto c = 0; c < 3; ++c)
    {
        w.put(512 - 20, 9);
        w.put(0, 1);
    }
    for (auto i = 0; i < 16; ++i)
        w.put(i == 0 ? 0 : 15, i == 0 ? 3 : 4);
    CHECK(w.pos == 128);

    // 10 unquantizes to 336, and 336 * 31 >> 5 is 325: half 0x0145, the subnormal 325 * 2^-24.
    auto const out = decode_bc6h(w.bytes, true);
    auto const magnitude = 325.0f / 16777216.0f;
    CHECK(out.rgba[0] == magnitude);
    CHECK(out.rgba[4] == -magnitude);
    CHECK(out.rgba[7] == 1.0f);
}

TEST("bcn - the float decoders keep the exact interpolants")
{
    // r = 1 widens to 8, and two thirds of 8 is 16/3: the 8-bit decode rounds it to 5, the float one keeps it.
    auto const block = make_color_block(0x0800, 0x0000, texel_order);
    auto const exact = bcn::decode_bc1_f32(bcn::small_block(block));
    CHECK(exact.rgba[2 * 4 + 0] == 16.0f / 765.0f);
    CHECK(exact.rgba[2 * 4 + 3] == 1.0f);

    // Three-color mode: index 3 is transparent black in floats too.
    auto const three = bcn::decode_bc1_f32(bcn::small_block(make_color_block(0x001F, 0xF800, texel_order)));
    CHECK(three.rgba[3 * 4 + 3] == 0.0f);

    // BC3 alpha in sevenths: (6 * 200 + 10) / 7, unrounded.
    auto const bc3 = bcn::decode_bc3_f32(
        bcn::large_block(concat(make_channel_block(200, 10, channel_order), make_color_block(0xFFFF, 0, texel_order))));
    CHECK(bc3.rgba[2 * 4 + 3] == 1210.0f / 1785.0f);

    // Signed BC5 in -1..1, unbiased.
    int const zeros[16] = {};
    auto const bc5 = bcn::decode_bc5_f32(
        bcn::large_block(concat(make_channel_block(0x81, 0x81, zeros), make_channel_block(0x7F, 0x7F, zeros))), true);
    CHECK(bc5.rgba[0] == -1.0f);
    CHECK(bc5.rgba[1] == 1.0f);
}
