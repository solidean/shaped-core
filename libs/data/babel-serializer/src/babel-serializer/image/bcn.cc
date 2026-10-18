#include <babel-serializer/image/bcn.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/math/bit.hh>
#include <typed-geometry/scalar/half_float.hh>

namespace babel::bcn
{
namespace
{
// Shared tables
// -------------------------------------------------------------------------------------------------

// The lookup tables stay C arrays on purpose: constexpr, transcribed from the specification row by row, and never
// passed anywhere a size could be lost.

/// The 64 two-subset partitions of BC6H and BC7, bit i set where texel i is in subset 1.
constexpr u16 partitions_2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

/// The 64 three-subset partitions of BC7, the subset of each texel.
constexpr u8 partitions_3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

/// The texel whose index drops its top bit in subset 1 of a two-subset partition; subset 0's is texel 0.
constexpr u8 anchors_2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};

/// The anchor texels of subsets 1 and 2 in a three-subset partition.
constexpr u8 anchors_3a[64] = {
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8, 6,  6, 6,  5,  3,  3,  3, 3,  8, 15, 3, 3, 6, 10, 5, 8,  8, 6,  8,  5,  15, 15,
    8, 15, 3,  5,  6, 10, 8,  15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5,  5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};
constexpr u8 anchors_3b[64] = {
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

/// Interpolation weights out of 64, by index width.
constexpr int weights_2[4] = {0, 21, 43, 64};
constexpr int weights_3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int weights_4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

int weight_of(int index_bits, int index)
{
    if (index_bits == 2)
        return weights_2[index];
    if (index_bits == 3)
        return weights_3[index];
    return weights_4[index];
}

/// A block as the plain span the byte loads take.
template <isize N>
cc::span<byte const> bytes_of(cc::fixed_span<byte const, N> block)
{
    return cc::span<byte const>(block.data(), N);
}

/// Reads a 128-bit block least significant bit first, the order every field of BC6H and BC7 is stored in.
struct bit_reader
{
    u64 lo = 0;
    u64 hi = 0;
    int pos = 0;

    explicit bit_reader(large_block block)
      : lo(cc::load_bytes_le<u64>(bytes_of(block), 0)), hi(cc::load_bytes_le<u64>(bytes_of(block), 8))
    {
    }

    /// The next `count` bits, `count` at most 32.
    u32 read(int count)
    {
        if (count == 0)
            return 0;
        auto const mask = count == 32 ? ~u64(0) >> 32 : (u64(1) << count) - 1;
        auto value = u64(0);
        if (pos >= 64)
            value = hi >> (pos - 64);
        else if (pos + count <= 64)
            value = lo >> pos;
        else
            value = (lo >> pos) | (hi << (64 - pos));
        pos += count;
        return u32(value & mask);
    }
};

// BC1 - BC5
// -------------------------------------------------------------------------------------------------

/// An RGB565 endpoint widened to 8 bits a channel by replicating its top bits, the way every decoder widens it.
cc::fixed_array<int, 3> expand_565(u16 c)
{
    auto const r = int(c >> 11) & 31;
    auto const g = int(c >> 5) & 63;
    auto const b = int(c) & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

/// The four colors of a BC1-style block as exact fractions over one denominator, and the entry each texel takes.
/// Keeping the fractions is what lets the 8-bit and the float decode come from one computation.
struct color_palette
{
    cc::fixed_array<cc::fixed_array<int, 4>, 4> numerators = {}; // [entry][r, g, b, a], 0..255 units
    int denominator = 1;
    cc::fixed_array<u8, 16> indices = {};
};

/// The color half of a BC1, BC2 or BC3 block; only BC1 may use the three-color mode.
color_palette read_color(small_block block, bool allow_three_color)
{
    auto const c0 = cc::load_bytes_le<u16>(bytes_of(block), 0);
    auto const c1 = cc::load_bytes_le<u16>(bytes_of(block), 2);
    auto const indices = cc::load_bytes_le<u32>(bytes_of(block), 4);
    auto const e0 = expand_565(c0);
    auto const e1 = expand_565(c1);

    auto p = color_palette{};
    if (!allow_three_color || c0 > c1)
    {
        p.denominator = 3;
        for (auto c = 0; c < 3; ++c)
        {
            p.numerators[0][c] = 3 * e0[c];
            p.numerators[1][c] = 3 * e1[c];
            p.numerators[2][c] = 2 * e0[c] + e1[c];
            p.numerators[3][c] = e0[c] + 2 * e1[c];
        }
        for (auto e = 0; e < 4; ++e)
            p.numerators[e][3] = 3 * 255;
    }
    else
    {
        // Entry 3 stays transparent black.
        p.denominator = 2;
        for (auto c = 0; c < 3; ++c)
        {
            p.numerators[0][c] = 2 * e0[c];
            p.numerators[1][c] = 2 * e1[c];
            p.numerators[2][c] = e0[c] + e1[c];
        }
        for (auto e = 0; e < 3; ++e)
            p.numerators[e][3] = 2 * 255;
    }

    for (auto i = 0; i < 16; ++i)
        p.indices[i] = u8((indices >> (2 * i)) & 3u);
    return p;
}

/// The eight values of a BC4 block as exact fractions over one denominator, and the entry each texel takes.
struct channel_palette
{
    cc::fixed_array<int, 8> numerators = {}; // 0..255 units unsigned, -127..127 signed
    int denominator = 1;
    cc::fixed_array<u8, 16> indices = {};
};

channel_palette read_channel(small_block block, bool is_signed)
{
    // The endpoints are compared as stored, and only then is a signed -128 read as -127.
    // That order is DirectXTex's, and it decides the mode of the one pair it matters for.
    auto e0 = is_signed ? int(i8(block[0])) : int(u8(block[0]));
    auto e1 = is_signed ? int(i8(block[1])) : int(u8(block[1]));
    auto const eight_values = e0 > e1;
    if (is_signed)
    {
        e0 = e0 < -127 ? -127 : e0;
        e1 = e1 < -127 ? -127 : e1;
    }

    auto p = channel_palette{};
    if (eight_values)
    {
        p.denominator = 7;
        p.numerators[0] = 7 * e0;
        p.numerators[1] = 7 * e1;
        for (auto k = 1; k <= 6; ++k)
            p.numerators[k + 1] = (7 - k) * e0 + k * e1;
    }
    else
    {
        p.denominator = 5;
        p.numerators[0] = 5 * e0;
        p.numerators[1] = 5 * e1;
        for (auto k = 1; k <= 4; ++k)
            p.numerators[k + 1] = (5 - k) * e0 + k * e1;
        p.numerators[6] = 5 * (is_signed ? -127 : 0);
        p.numerators[7] = 5 * (is_signed ? 127 : 255);
    }

    // 48 bits of indices, 3 per texel, after the two endpoint bytes.
    auto const indices = cc::load_bytes_le<u64>(bytes_of(block), 0) >> 16;
    for (auto i = 0; i < 16; ++i)
        p.indices[i] = u8((indices >> (3 * i)) & 7u);
    return p;
}

/// Rounds `numerator / denominator` to nearest, symmetrically about zero.
/// Only the denominator 2 has ties, and its numerators are never negative, so ties round up.
int divide_rounded(int numerator, int denominator)
{
    if (numerator >= 0)
        return (numerator + denominator / 2) / denominator;
    return -((-numerator + denominator / 2) / denominator);
}

/// An unsigned 8-bit value from a signed BC4 interpolant `numerator / denominator`, -127 at 0 and 127 at 255.
/// Biases the exact fraction and rounds once, so the result is the float decode rounded to 8 bits.
u8 bias_signed(int numerator, int denominator)
{
    auto const biased = (numerator + 127 * denominator) * 255;
    auto const scale = 254 * denominator;
    return u8((biased + scale / 2) / scale);
}

/// What a signed 8-bit decode writes into a channel the format does not store: 0, biased.
constexpr u8 signed_zero = 128;

/// One channel of a BC4 block as the 8-bit decode stores it, biased when signed.
cc::fixed_array<u8, 16> decode_bc4_u8(small_block block, bool is_signed)
{
    auto const p = read_channel(block, is_signed);
    auto out = cc::fixed_array<u8, 16>{};
    for (auto i = 0; i < 16; ++i)
    {
        auto const numerator = p.numerators[p.indices[i]];
        out[i] = is_signed ? bias_signed(numerator, p.denominator) : u8(divide_rounded(numerator, p.denominator));
    }
    return out;
}

/// A sign-extended value of `bits` bits.
int sign_extend(int value, int bits)
{
    auto const sign = 1 << (bits - 1);
    auto const masked = value & ((1 << bits) - 1);
    return (masked ^ sign) - sign;
}

// BC6H
// -------------------------------------------------------------------------------------------------

/// The fields a BC6H header scatters its bits into: endpoint e's channel c is field e * 3 + c, and the partition
/// index is the thirteenth.
enum bc6h_field : u8
{
    rw,
    gw,
    bw,
    rx,
    gx,
    bx,
    ry,
    gy,
    by,
    rz,
    gz,
    bz,
    d,
};

/// A run of one field's bits in stream order, from bit `first` to bit `last`, either way round.
struct bc6h_run
{
    bc6h_field field = rw;
    u8 first = 0;
    u8 last = 0;
};

/// One of BC6H's fourteen modes, with its header layout after the mode bits.
struct bc6h_mode
{
    u8 code = 0;
    bool transformed = false;
    int regions = 1;
    int precision = 0;
    int delta[3] = {};
    bc6h_run runs[32] = {};
};

// The layouts are the specification's, written field by field in the order the bits are stored.
// A run whose `first` exceeds its `last` is stored most significant bit first, as modes 0x0B and 0x0F do.
constexpr bc6h_mode bc6h_modes[] = {
    {0x00, true, 2, 10, {5, 5, 5}, {{gy, 4, 4}, {by, 4, 4}, {bz, 4, 4}, {rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9}, {rx, 0, 4},
                                    {gz, 4, 4}, {gy, 0, 3}, {gx, 0, 4}, {bz, 0, 0}, {gz, 0, 3}, {bx, 0, 4}, {bz, 1, 1},
                                    {by, 0, 3}, {ry, 0, 4}, {bz, 2, 2}, {rz, 0, 4}, {bz, 3, 3}, {d, 0, 4}}},
    {0x01, true, 2, 7, {6, 6, 6}, {{gy, 5, 5}, {gz, 4, 4}, {gz, 5, 5}, {rw, 0, 6}, {bz, 0, 0}, {bz, 1, 1},
                                   {by, 4, 4}, {gw, 0, 6}, {by, 5, 5}, {bz, 2, 2}, {gy, 4, 4}, {bw, 0, 6},
                                   {bz, 3, 3}, {bz, 5, 5}, {bz, 4, 4}, {rx, 0, 5}, {gy, 0, 3}, {gx, 0, 5},
                                   {gz, 0, 3}, {bx, 0, 5}, {by, 0, 3}, {ry, 0, 5}, {rz, 0, 5}, {d, 0, 4}}},
    {0x02,
     true,
     2,
     11,
     {5, 4, 4},
     {{rw, 0, 9},
      {gw, 0, 9},
      {bw, 0, 9},
      {rx, 0, 4},
      {rw, 10, 10},
      {gy, 0, 3},
      {gx, 0, 3},
      {gw, 10, 10},
      {bz, 0, 0},
      {gz, 0, 3},
      {bx, 0, 3},
      {bw, 10, 10},
      {bz, 1, 1},
      {by, 0, 3},
      {ry, 0, 4},
      {bz, 2, 2},
      {rz, 0, 4},
      {bz, 3, 3},
      {d, 0, 4}}},
    {0x06, true, 2, 11, {4, 5, 4}, {{rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9},   {rx, 0, 3}, {rw, 10, 10}, {gz, 4, 4},
                                    {gy, 0, 3}, {gx, 0, 4}, {gw, 10, 10}, {gz, 0, 3}, {bx, 0, 3},   {bw, 10, 10},
                                    {bz, 1, 1}, {by, 0, 3}, {ry, 0, 3},   {bz, 0, 0}, {bz, 2, 2},   {rz, 0, 3},
                                    {gy, 4, 4}, {bz, 3, 3}, {d, 0, 4}}},
    {0x0A, true, 2, 11, {4, 4, 5}, {{rw, 0, 9},   {gw, 0, 9}, {bw, 0, 9},   {rx, 0, 3}, {rw, 10, 10}, {by, 4, 4},
                                    {gy, 0, 3},   {gx, 0, 3}, {gw, 10, 10}, {bz, 0, 0}, {gz, 0, 3},   {bx, 0, 4},
                                    {bw, 10, 10}, {by, 0, 3}, {ry, 0, 3},   {bz, 1, 1}, {bz, 2, 2},   {rz, 0, 3},
                                    {bz, 4, 4},   {bz, 3, 3}, {d, 0, 4}}},
    {0x0E, true, 2, 9, {5, 5, 5}, {{rw, 0, 8}, {by, 4, 4}, {gw, 0, 8}, {gy, 4, 4}, {bw, 0, 8}, {bz, 4, 4}, {rx, 0, 4},
                                   {gz, 4, 4}, {gy, 0, 3}, {gx, 0, 4}, {bz, 0, 0}, {gz, 0, 3}, {bx, 0, 4}, {bz, 1, 1},
                                   {by, 0, 3}, {ry, 0, 4}, {bz, 2, 2}, {rz, 0, 4}, {bz, 3, 3}, {d, 0, 4}}},
    {0x12, true, 2, 8, {6, 5, 5}, {{rw, 0, 7}, {gz, 4, 4}, {by, 4, 4}, {gw, 0, 7}, {bz, 2, 2}, {gy, 4, 4}, {bw, 0, 7},
                                   {bz, 3, 3}, {bz, 4, 4}, {rx, 0, 5}, {gy, 0, 3}, {gx, 0, 4}, {bz, 0, 0}, {gz, 0, 3},
                                   {bx, 0, 4}, {bz, 1, 1}, {by, 0, 3}, {ry, 0, 5}, {rz, 0, 5}, {d, 0, 4}}},
    {0x16, true, 2, 8, {5, 6, 5}, {{rw, 0, 7}, {bz, 0, 0}, {by, 4, 4}, {gw, 0, 7}, {gy, 5, 5}, {gy, 4, 4},
                                   {bw, 0, 7}, {gz, 5, 5}, {bz, 4, 4}, {rx, 0, 4}, {gz, 4, 4}, {gy, 0, 3},
                                   {gx, 0, 5}, {gz, 0, 3}, {bx, 0, 4}, {bz, 1, 1}, {by, 0, 3}, {ry, 0, 4},
                                   {bz, 2, 2}, {rz, 0, 4}, {bz, 3, 3}, {d, 0, 4}}},
    {0x1A, true, 2, 8, {5, 5, 6}, {{rw, 0, 7}, {bz, 1, 1}, {by, 4, 4}, {gw, 0, 7}, {by, 5, 5}, {gy, 4, 4},
                                   {bw, 0, 7}, {bz, 5, 5}, {bz, 4, 4}, {rx, 0, 4}, {gz, 4, 4}, {gy, 0, 3},
                                   {gx, 0, 4}, {bz, 0, 0}, {gz, 0, 3}, {bx, 0, 5}, {by, 0, 3}, {ry, 0, 4},
                                   {bz, 2, 2}, {rz, 0, 4}, {bz, 3, 3}, {d, 0, 4}}},
    {0x1E, false, 2, 6, {6, 6, 6}, {{rw, 0, 5}, {gz, 4, 4}, {bz, 0, 0}, {bz, 1, 1}, {by, 4, 4}, {gw, 0, 5},
                                    {gy, 5, 5}, {by, 5, 5}, {bz, 2, 2}, {gy, 4, 4}, {bw, 0, 5}, {gz, 5, 5},
                                    {bz, 3, 3}, {bz, 5, 5}, {bz, 4, 4}, {rx, 0, 5}, {gy, 0, 3}, {gx, 0, 5},
                                    {gz, 0, 3}, {bx, 0, 5}, {by, 0, 3}, {ry, 0, 5}, {rz, 0, 5}, {d, 0, 4}}},
    {0x03, false, 1, 10, {10, 10, 10}, {{rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9}, {rx, 0, 9}, {gx, 0, 9}, {bx, 0, 9}}},
    {0x07,
     true,
     1,
     11,
     {9, 9, 9},
     {{rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9}, {rx, 0, 8}, {rw, 10, 10}, {gx, 0, 8}, {gw, 10, 10}, {bx, 0, 8}, {bw, 10, 10}}},
    {0x0B,
     true,
     1,
     12,
     {8, 8, 8},
     {{rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9}, {rx, 0, 7}, {rw, 11, 10}, {gx, 0, 7}, {gw, 11, 10}, {bx, 0, 7}, {bw, 11, 10}}},
    {0x0F,
     true,
     1,
     16,
     {4, 4, 4},
     {{rw, 0, 9}, {gw, 0, 9}, {bw, 0, 9}, {rx, 0, 3}, {rw, 15, 10}, {gx, 0, 3}, {gw, 15, 10}, {bx, 0, 3}, {bw, 15, 10}}},
};

/// An endpoint widened to the 16-bit range interpolation runs in.
int bc6h_unquantize(int value, int bits, bool is_signed)
{
    if (!is_signed)
    {
        if (bits >= 15 || value == 0)
            return value;
        if (value == (1 << bits) - 1)
            return 0xFFFF;
        return ((value << 16) + 0x8000) >> bits;
    }

    if (bits >= 16)
        return value;
    auto const negative = value < 0;
    auto const magnitude = negative ? -value : value;
    auto unquantized = 0;
    if (magnitude == 0)
        unquantized = 0;
    else if (magnitude >= (1 << (bits - 1)) - 1)
        unquantized = 0x7FFF;
    else
        unquantized = ((magnitude << 15) + 0x4000) >> (bits - 1);
    return negative ? -unquantized : unquantized;
}

/// An interpolated value scaled into a half float's bit pattern, then widened.
float bc6h_finish(int value, bool is_signed)
{
    auto bits = u16(0);
    if (!is_signed)
        bits = u16((value * 31) >> 6);
    else if (value < 0)
        bits = u16(0x8000 | (((-value) * 31) >> 5));
    else
        bits = u16((value * 31) >> 5);
    return float(tg::f16::make_from_bits(bits));
}

} // namespace

// BC1 - BC5
// -------------------------------------------------------------------------------------------------

rgba8_block decode_bc1(small_block block)
{
    auto const p = read_color(block, true);
    auto out = rgba8_block{};
    for (auto i = 0; i < 16; ++i)
        for (auto c = 0; c < 4; ++c)
            out.rgba[i * 4 + c] = u8(divide_rounded(p.numerators[p.indices[i]][c], p.denominator));
    return out;
}

rgba32f_block decode_bc1_f32(small_block block)
{
    auto const p = read_color(block, true);
    auto const denominator = float(p.denominator * 255);
    auto out = rgba32f_block{};
    for (auto i = 0; i < 16; ++i)
        for (auto c = 0; c < 4; ++c)
            out.rgba[i * 4 + c] = float(p.numerators[p.indices[i]][c]) / denominator;
    return out;
}

rgba8_block decode_bc2(large_block block)
{
    auto const p = read_color(small_block(block.data() + 8), false);
    auto const alpha = cc::load_bytes_le<u64>(bytes_of(block), 0);
    auto out = rgba8_block{};
    for (auto i = 0; i < 16; ++i)
    {
        for (auto c = 0; c < 3; ++c)
            out.rgba[i * 4 + c] = u8(divide_rounded(p.numerators[p.indices[i]][c], p.denominator));
        out.rgba[i * 4 + 3] = u8(((alpha >> (4 * i)) & 15u) * 17u);
    }
    return out;
}

rgba32f_block decode_bc2_f32(large_block block)
{
    auto const p = read_color(small_block(block.data() + 8), false);
    auto const denominator = float(p.denominator * 255);
    auto const alpha = cc::load_bytes_le<u64>(bytes_of(block), 0);
    auto out = rgba32f_block{};
    for (auto i = 0; i < 16; ++i)
    {
        for (auto c = 0; c < 3; ++c)
            out.rgba[i * 4 + c] = float(p.numerators[p.indices[i]][c]) / denominator;
        out.rgba[i * 4 + 3] = float((alpha >> (4 * i)) & 15u) / 15.0f;
    }
    return out;
}

cc::fixed_array<i16, 16> decode_bc4_channel(small_block block, bool is_signed)
{
    auto const p = read_channel(block, is_signed);
    auto out = cc::fixed_array<i16, 16>{};
    for (auto i = 0; i < 16; ++i)
        out[i] = i16(divide_rounded(p.numerators[p.indices[i]], p.denominator));
    return out;
}

rgba8_block decode_bc3(large_block block)
{
    auto const p = read_color(small_block(block.data() + 8), false);
    auto const alpha = decode_bc4_channel(small_block(block.data()), false);
    auto out = rgba8_block{};
    for (auto i = 0; i < 16; ++i)
    {
        for (auto c = 0; c < 3; ++c)
            out.rgba[i * 4 + c] = u8(divide_rounded(p.numerators[p.indices[i]][c], p.denominator));
        out.rgba[i * 4 + 3] = u8(alpha[i]);
    }
    return out;
}

rgba32f_block decode_bc3_f32(large_block block)
{
    auto const p = read_color(small_block(block.data() + 8), false);
    auto const alpha = read_channel(small_block(block.data()), false);
    auto const denominator = float(p.denominator * 255);
    auto const alpha_denominator = float(alpha.denominator * 255);
    auto out = rgba32f_block{};
    for (auto i = 0; i < 16; ++i)
    {
        for (auto c = 0; c < 3; ++c)
            out.rgba[i * 4 + c] = float(p.numerators[p.indices[i]][c]) / denominator;
        out.rgba[i * 4 + 3] = float(alpha.numerators[alpha.indices[i]]) / alpha_denominator;
    }
    return out;
}

rgba8_block decode_bc4(small_block block, bool is_signed)
{
    auto const red = decode_bc4_u8(block, is_signed);
    auto const unused = is_signed ? signed_zero : u8(0);
    auto out = rgba8_block{};
    for (auto i = 0; i < 16; ++i)
    {
        out.rgba[i * 4 + 0] = red[i];
        out.rgba[i * 4 + 1] = unused;
        out.rgba[i * 4 + 2] = unused;
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

rgba32f_block decode_bc4_f32(small_block block, bool is_signed)
{
    auto const p = read_channel(block, is_signed);
    auto const denominator = float(p.denominator * (is_signed ? 127 : 255));
    auto out = rgba32f_block{};
    for (auto i = 0; i < 16; ++i)
    {
        out.rgba[i * 4 + 0] = float(p.numerators[p.indices[i]]) / denominator;
        out.rgba[i * 4 + 3] = 1.0f;
    }
    return out;
}

rgba8_block decode_bc5(large_block block, bool is_signed)
{
    auto const red = decode_bc4_u8(small_block(block.data()), is_signed);
    auto const green = decode_bc4_u8(small_block(block.data() + 8), is_signed);
    auto const unused = is_signed ? signed_zero : u8(0);
    auto out = rgba8_block{};
    for (auto i = 0; i < 16; ++i)
    {
        out.rgba[i * 4 + 0] = red[i];
        out.rgba[i * 4 + 1] = green[i];
        out.rgba[i * 4 + 2] = unused;
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

rgba32f_block decode_bc5_f32(large_block block, bool is_signed)
{
    auto out = decode_bc4_f32(small_block(block.data()), is_signed);
    auto const green = decode_bc4_f32(small_block(block.data() + 8), is_signed);
    for (auto i = 0; i < 16; ++i)
        out.rgba[i * 4 + 1] = green.rgba[i * 4];
    return out;
}

// BC7
// -------------------------------------------------------------------------------------------------

rgba8_block decode_bc7(large_block block)
{
    struct mode_info
    {
        int subsets = 1;
        int partition_bits = 0;
        int rotation_bits = 0;
        int selector_bits = 0;
        int color_bits = 0;
        int alpha_bits = 0;
        bool endpoint_pbits = false;
        bool shared_pbits = false;
        int index_bits = 0;
        int index_bits_2 = 0;
    };
    constexpr mode_info modes[8] = {
        {3, 4, 0, 0, 4, 0, true, false, 3, 0},  {2, 6, 0, 0, 6, 0, false, true, 3, 0},
        {3, 6, 0, 0, 5, 0, false, false, 2, 0}, {2, 6, 0, 0, 7, 0, true, false, 2, 0},
        {1, 0, 2, 1, 5, 6, false, false, 2, 3}, {1, 0, 2, 0, 7, 8, false, false, 2, 2},
        {1, 0, 0, 0, 7, 7, true, false, 4, 0},  {2, 6, 0, 0, 5, 5, true, false, 2, 0},
    };

    auto out = rgba8_block{};
    auto const first = u8(block[0]);
    if (first == 0)
        return out; // a reserved mode is transparent black

    auto const mode = cc::count_trailing_zeroes(u32(first));
    auto const& m = modes[mode];

    auto bits = bit_reader(block);
    (void)bits.read(mode + 1);
    auto const partition = int(bits.read(m.partition_bits));
    auto const rotation = int(bits.read(m.rotation_bits));
    auto const selector = int(bits.read(m.selector_bits));

    auto const endpoint_count = m.subsets * 2;
    auto endpoints = cc::fixed_array<cc::fixed_array<int, 4>, 6>{};
    for (auto c = 0; c < 3; ++c)
        for (auto e = 0; e < endpoint_count; ++e)
            endpoints[e][c] = int(bits.read(m.color_bits));
    for (auto e = 0; e < endpoint_count; ++e)
        endpoints[e][3] = m.alpha_bits > 0 ? int(bits.read(m.alpha_bits)) : 255;

    auto color_bits = m.color_bits;
    auto alpha_bits = m.alpha_bits;
    if (m.endpoint_pbits || m.shared_pbits)
    {
        auto pbits = cc::fixed_array<int, 6>{};
        if (m.endpoint_pbits)
            for (auto e = 0; e < endpoint_count; ++e)
                pbits[e] = int(bits.read(1));
        else
            for (auto s = 0; s < m.subsets; ++s)
            {
                pbits[s * 2] = int(bits.read(1));
                pbits[s * 2 + 1] = pbits[s * 2];
            }

        for (auto e = 0; e < endpoint_count; ++e)
            for (auto c = 0; c < (m.alpha_bits > 0 ? 4 : 3); ++c)
                endpoints[e][c] = (endpoints[e][c] << 1) | pbits[e];
        color_bits += 1;
        alpha_bits = m.alpha_bits > 0 ? alpha_bits + 1 : 0;
    }

    // Widen to 8 bits by replicating the top bits into the bottom ones.
    for (auto e = 0; e < endpoint_count; ++e)
        for (auto c = 0; c < 4; ++c)
        {
            auto const width = c < 3 ? color_bits : alpha_bits;
            if (width == 0)
                continue;
            auto const v = endpoints[e][c] << (8 - width);
            endpoints[e][c] = v | (v >> width);
        }

    auto const subset_of = [&](int texel) -> int
    {
        if (m.subsets == 2)
            return (partitions_2[partition] >> texel) & 1;
        if (m.subsets == 3)
            return partitions_3[partition][texel];
        return 0;
    };
    auto const is_anchor = [&](int texel) -> bool
    {
        if (texel == 0)
            return true;
        if (m.subsets == 2)
            return texel == anchors_2[partition];
        if (m.subsets == 3)
            return texel == anchors_3a[partition] || texel == anchors_3b[partition];
        return false;
    };

    auto indices = cc::fixed_array<int, 16>{};
    for (auto i = 0; i < 16; ++i)
        indices[i] = int(bits.read(is_anchor(i) ? m.index_bits - 1 : m.index_bits));
    auto indices_2 = cc::fixed_array<int, 16>{};
    if (m.index_bits_2 > 0)
        for (auto i = 0; i < 16; ++i)
            indices_2[i] = int(bits.read(i == 0 ? m.index_bits_2 - 1 : m.index_bits_2));

    for (auto i = 0; i < 16; ++i)
    {
        auto const s = subset_of(i);
        auto color_weight = weight_of(m.index_bits, indices[i]);
        auto alpha_weight = color_weight;
        if (m.index_bits_2 > 0)
        {
            alpha_weight = weight_of(m.index_bits_2, indices_2[i]);
            if (selector != 0)
            {
                color_weight = weight_of(m.index_bits_2, indices_2[i]);
                alpha_weight = weight_of(m.index_bits, indices[i]);
            }
        }

        auto texel = cc::fixed_array<int, 4>{};
        for (auto c = 0; c < 4; ++c)
        {
            auto const w = c < 3 ? color_weight : alpha_weight;
            texel[c] = (endpoints[s * 2][c] * (64 - w) + endpoints[s * 2 + 1][c] * w + 32) >> 6;
        }

        // Modes 4 and 5 may store one color channel in the alpha slot, which swaps back here.
        if (rotation > 0)
        {
            auto const swapped = texel[rotation - 1];
            texel[rotation - 1] = texel[3];
            texel[3] = swapped;
        }

        for (auto c = 0; c < 4; ++c)
            out.rgba[i * 4 + c] = u8(texel[c]);
    }
    return out;
}

// BC6H
// -------------------------------------------------------------------------------------------------

rgba32f_block decode_bc6h(large_block block, bool is_signed)
{
    auto out = rgba32f_block{};
    auto bits = bit_reader(block);

    auto code = bits.read(2);
    if (code >= 2)
        code |= bits.read(3) << 2;

    auto const* mode = static_cast<bc6h_mode const*>(nullptr);
    for (auto const& candidate : bc6h_modes)
        if (candidate.code == code)
            mode = &candidate;
    if (mode == nullptr)
    {
        // A reserved mode is opaque black: BC6H has no alpha, and the specification requires it.
        for (auto i = 0; i < 16; ++i)
            out.rgba[i * 4 + 3] = 1.0f;
        return out;
    }

    auto fields = cc::fixed_array<int, 13>{};
    for (auto const& run : mode->runs)
    {
        if (run.first == 0 && run.last == 0 && run.field == rw)
            break; // the zero-initialized tail of the run table
        auto const step = run.first <= run.last ? 1 : -1;
        for (auto b = int(run.first);; b += step)
        {
            fields[run.field] |= int(bits.read(1)) << b;
            if (b == int(run.last))
                break;
        }
    }

    auto const endpoint_count = mode->regions * 2;
    auto endpoints = cc::fixed_array<cc::fixed_array<int, 3>, 4>{};
    for (auto e = 0; e < endpoint_count; ++e)
        for (auto c = 0; c < 3; ++c)
            endpoints[e][c] = fields[e * 3 + c];

    for (auto c = 0; c < 3; ++c)
    {
        if (is_signed)
            endpoints[0][c] = sign_extend(endpoints[0][c], mode->precision);
        if (is_signed || mode->transformed)
            for (auto e = 1; e < endpoint_count; ++e)
                endpoints[e][c] = sign_extend(endpoints[e][c], mode->delta[c]);
        if (mode->transformed)
            for (auto e = 1; e < endpoint_count; ++e)
            {
                endpoints[e][c] = (endpoints[0][c] + endpoints[e][c]) & ((1 << mode->precision) - 1);
                // Per the specification, so a delta-coded endpoint can be negative.
                if (is_signed)
                    endpoints[e][c] = sign_extend(endpoints[e][c], mode->precision);
            }
    }

    for (auto e = 0; e < endpoint_count; ++e)
        for (auto c = 0; c < 3; ++c)
            endpoints[e][c] = bc6h_unquantize(endpoints[e][c], mode->precision, is_signed);

    auto const partition = fields[d];
    auto const index_bits = mode->regions == 2 ? 3 : 4;
    for (auto i = 0; i < 16; ++i)
    {
        auto const anchor = i == 0 || (mode->regions == 2 && i == anchors_2[partition]);
        auto const index = int(bits.read(anchor ? index_bits - 1 : index_bits));
        auto const region = mode->regions == 2 ? (partitions_2[partition] >> i) & 1 : 0;
        auto const w = weight_of(index_bits, index);

        for (auto c = 0; c < 3; ++c)
        {
            auto const a = endpoints[region * 2][c];
            auto const b = endpoints[region * 2 + 1][c];
            out.rgba[i * 4 + c] = bc6h_finish((a * (64 - w) + b * w + 32) >> 6, is_signed);
        }
        out.rgba[i * 4 + 3] = 1.0f;
    }
    return out;
}
} // namespace babel::bcn
