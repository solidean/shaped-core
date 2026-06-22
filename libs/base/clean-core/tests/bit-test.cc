#include <clean-core/bit.hh>

#include <nexus/test.hh>

#include <iostream>

using namespace cc::primitive_defines;

// =========================================================================================================
// Bit representation tests
// =========================================================================================================

TEST("bit - bit_cast preserves bit representation")
{
    SECTION("float to u32 and back")
    {
        f32 const original = 3.14f;
        u32 const bits = cc::bit_cast<u32>(original);
        f32 const restored = cc::bit_cast<f32>(bits);
        CHECK(restored == original);
    }

    SECTION("double to u64 and back")
    {
        f64 const original = 2.718281828459045;
        u64 const bits = cc::bit_cast<u64>(original);
        f64 const restored = cc::bit_cast<f64>(bits);
        CHECK(restored == original);
    }

    SECTION("known float bit patterns")
    {
        CHECK(cc::bit_cast<u32>(1.0f) == 0x3F800000u);
        CHECK(cc::bit_cast<u32>(0.0f) == 0x00000000u);
        CHECK(cc::bit_cast<u32>(-0.0f) == 0x80000000u);
    }

    SECTION("integer roundtrip")
    {
        u32 const original = 0xDEADBEEFu;
        f32 const as_float = cc::bit_cast<f32>(original);
        u32 const restored = cc::bit_cast<u32>(as_float);
        CHECK(restored == original);
    }
}

// =========================================================================================================
// Power of two operations tests
// =========================================================================================================

TEST("bit - has_single_bit identifies powers of two")
{
    SECTION("powers of two (u8)")
    {
        CHECK(cc::has_single_bit(u8(1)));
        CHECK(cc::has_single_bit(u8(2)));
        CHECK(cc::has_single_bit(u8(4)));
        CHECK(cc::has_single_bit(u8(8)));
        CHECK(cc::has_single_bit(u8(16)));
        CHECK(cc::has_single_bit(u8(32)));
        CHECK(cc::has_single_bit(u8(64)));
        CHECK(cc::has_single_bit(u8(128)));
    }

    SECTION("non-powers of two (u8)")
    {
        CHECK(!cc::has_single_bit(u8(0)));
        CHECK(!cc::has_single_bit(u8(3)));
        CHECK(!cc::has_single_bit(u8(5)));
        CHECK(!cc::has_single_bit(u8(6)));
        CHECK(!cc::has_single_bit(u8(7)));
        CHECK(!cc::has_single_bit(u8(9)));
        CHECK(!cc::has_single_bit(u8(15)));
        CHECK(!cc::has_single_bit(u8(255)));
    }

    SECTION("powers of two (u32)")
    {
        CHECK(cc::has_single_bit(u32(1)));
        CHECK(cc::has_single_bit(u32(1024)));
        CHECK(cc::has_single_bit(u32(65536)));
        CHECK(cc::has_single_bit(u32(1u << 31)));
    }

    SECTION("edge cases (u64)")
    {
        CHECK(cc::has_single_bit(u64(1)));
        CHECK(cc::has_single_bit(u64(1ull << 63)));
        CHECK(!cc::has_single_bit(u64(0)));
        CHECK(!cc::has_single_bit(u64(3)));
    }
}

TEST("bit - bit_ceil rounds up to power of two")
{
    SECTION("u8 values")
    {
        CHECK(cc::bit_ceil(u8(0)) == 1);
        CHECK(cc::bit_ceil(u8(1)) == 1);
        CHECK(cc::bit_ceil(u8(2)) == 2);
        CHECK(cc::bit_ceil(u8(3)) == 4);
        CHECK(cc::bit_ceil(u8(4)) == 4);
        CHECK(cc::bit_ceil(u8(5)) == 8);
        CHECK(cc::bit_ceil(u8(7)) == 8);
        CHECK(cc::bit_ceil(u8(8)) == 8);
        CHECK(cc::bit_ceil(u8(9)) == 16);
        CHECK(cc::bit_ceil(u8(15)) == 16);
        CHECK(cc::bit_ceil(u8(16)) == 16);
        CHECK(cc::bit_ceil(u8(17)) == 32);
        CHECK(cc::bit_ceil(u8(127)) == 128);
        CHECK(cc::bit_ceil(u8(128)) == 128);
    }

    SECTION("u16 values")
    {
        CHECK(cc::bit_ceil(u16(0)) == 1);
        CHECK(cc::bit_ceil(u16(1000)) == 1024);
        CHECK(cc::bit_ceil(u16(1024)) == 1024);
        CHECK(cc::bit_ceil(u16(1025)) == 2048);
        CHECK(cc::bit_ceil(u16(32767)) == 32768);
        CHECK(cc::bit_ceil(u16(32768)) == 32768);
    }

    SECTION("u32 values")
    {
        CHECK(cc::bit_ceil(u32(0)) == 1);
        CHECK(cc::bit_ceil(u32(100000)) == 131072);
        CHECK(cc::bit_ceil(u32(1u << 20)) == (1u << 20));
        CHECK(cc::bit_ceil(u32((1u << 20) + 1)) == (1u << 21));
    }

    SECTION("u64 values")
    {
        CHECK(cc::bit_ceil(u64(0)) == 1);
        CHECK(cc::bit_ceil(u64(1ull << 40)) == (1ull << 40));
        CHECK(cc::bit_ceil(u64((1ull << 40) + 1)) == (1ull << 41));
    }
}

TEST("bit - bit_floor rounds down to power of two")
{
    SECTION("u8 values")
    {
        CHECK(cc::bit_floor(u8(0)) == 0);
        CHECK(cc::bit_floor(u8(1)) == 1);
        CHECK(cc::bit_floor(u8(2)) == 2);
        CHECK(cc::bit_floor(u8(3)) == 2);
        CHECK(cc::bit_floor(u8(4)) == 4);
        CHECK(cc::bit_floor(u8(5)) == 4);
        CHECK(cc::bit_floor(u8(7)) == 4);
        CHECK(cc::bit_floor(u8(8)) == 8);
        CHECK(cc::bit_floor(u8(9)) == 8);
        CHECK(cc::bit_floor(u8(15)) == 8);
        CHECK(cc::bit_floor(u8(16)) == 16);
        CHECK(cc::bit_floor(u8(17)) == 16);
        CHECK(cc::bit_floor(u8(255)) == 128);
    }

    SECTION("u16 values")
    {
        CHECK(cc::bit_floor(u16(0)) == 0);
        CHECK(cc::bit_floor(u16(1000)) == 512);
        CHECK(cc::bit_floor(u16(1024)) == 1024);
        CHECK(cc::bit_floor(u16(1025)) == 1024);
        CHECK(cc::bit_floor(u16(65535)) == 32768);
    }

    SECTION("u32 values")
    {
        CHECK(cc::bit_floor(u32(0)) == 0);
        CHECK(cc::bit_floor(u32(100000)) == 65536);
        CHECK(cc::bit_floor(u32(1u << 20)) == (1u << 20));
        CHECK(cc::bit_floor(u32((1u << 20) + 1)) == (1u << 20));
    }

    SECTION("u64 values")
    {
        CHECK(cc::bit_floor(u64(0)) == 0);
        CHECK(cc::bit_floor(u64(1ull << 40)) == (1ull << 40));
        CHECK(cc::bit_floor(u64((1ull << 40) + 1)) == (1ull << 40));
    }
}

TEST("bit - bit_width counts required bits")
{
    SECTION("u8 values")
    {
        CHECK(cc::bit_width(u8(0)) == 0);
        CHECK(cc::bit_width(u8(1)) == 1);
        CHECK(cc::bit_width(u8(2)) == 2);
        CHECK(cc::bit_width(u8(3)) == 2);
        CHECK(cc::bit_width(u8(4)) == 3);
        CHECK(cc::bit_width(u8(5)) == 3);
        CHECK(cc::bit_width(u8(7)) == 3);
        CHECK(cc::bit_width(u8(8)) == 4);
        CHECK(cc::bit_width(u8(15)) == 4);
        CHECK(cc::bit_width(u8(16)) == 5);
        CHECK(cc::bit_width(u8(127)) == 7);
        CHECK(cc::bit_width(u8(128)) == 8);
        CHECK(cc::bit_width(u8(255)) == 8);
    }

    SECTION("u16 values")
    {
        CHECK(cc::bit_width(u16(0)) == 0);
        CHECK(cc::bit_width(u16(255)) == 8);
        CHECK(cc::bit_width(u16(256)) == 9);
        CHECK(cc::bit_width(u16(1023)) == 10);
        CHECK(cc::bit_width(u16(1024)) == 11);
        CHECK(cc::bit_width(u16(32767)) == 15);
        CHECK(cc::bit_width(u16(32768)) == 16);
        CHECK(cc::bit_width(u16(65535)) == 16);
    }

    SECTION("u32 values")
    {
        CHECK(cc::bit_width(u32(0)) == 0);
        CHECK(cc::bit_width(u32(0xFFu)) == 8);
        CHECK(cc::bit_width(u32(0xFFFFu)) == 16);
        CHECK(cc::bit_width(u32(0xFFFFFFu)) == 24);
        CHECK(cc::bit_width(u32(0xFFFFFFFFu)) == 32);
    }

    SECTION("u64 values")
    {
        CHECK(cc::bit_width(u64(0)) == 0);
        CHECK(cc::bit_width(u64(0xFFFFFFFFull)) == 32);
        CHECK(cc::bit_width(u64(0xFFFFFFFFFFFFFFFFull)) == 64);
    }
}

// =========================================================================================================
// Bit rotation tests
// =========================================================================================================

TEST("bit - bit_rotate_left shifts with wraparound")
{
    SECTION("u8 rotations")
    {
        // 0b10110010 rotated left by 2 => 0b11001010
        CHECK(cc::bit_rotate_left(u8(0b10110010), 2) == u8(0b11001010));

        // 0b10000000 rotated left by 1 => 0b00000001
        CHECK(cc::bit_rotate_left(u8(0b10000000), 1) == u8(0b00000001));

        // 0b11110000 rotated left by 4 => 0b00001111
        CHECK(cc::bit_rotate_left(u8(0b11110000), 4) == u8(0b00001111));

        // rotate by 0
        CHECK(cc::bit_rotate_left(u8(0b10101010), 0) == u8(0b10101010));

        // full rotation
        CHECK(cc::bit_rotate_left(u8(0b10101010), 8) == u8(0b10101010));

        // rotate by more than width
        CHECK(cc::bit_rotate_left(u8(0b10110010), 10) == u8(0b11001010)); // same as rotate by 2
    }

    SECTION("u16 rotations")
    {
        CHECK(cc::bit_rotate_left(u16(0x8000), 1) == u16(0x0001));
        CHECK(cc::bit_rotate_left(u16(0xFF00), 8) == u16(0x00FF));
        CHECK(cc::bit_rotate_left(u16(0x1234), 4) == u16(0x2341));
    }

    SECTION("u32 rotations")
    {
        CHECK(cc::bit_rotate_left(u32(0x80000000), 1) == u32(0x00000001));
        CHECK(cc::bit_rotate_left(u32(0xFF000000), 8) == u32(0x000000FF));
        CHECK(cc::bit_rotate_left(u32(0x12345678), 8) == u32(0x34567812));
    }

    SECTION("u64 rotations")
    {
        CHECK(cc::bit_rotate_left(u64(0x8000000000000000ull), 1) == u64(0x0000000000000001ull));
        CHECK(cc::bit_rotate_left(u64(0xFF00000000000000ull), 8) == u64(0x00000000000000FFull));
        CHECK(cc::bit_rotate_left(u64(0x123456789ABCDEF0ull), 16) == u64(0x56789ABCDEF01234ull));
    }

    SECTION("negative shift performs right rotation")
    {
        u8 const val = 0b10110010;
        CHECK(cc::bit_rotate_left(val, -2) == cc::bit_rotate_right(val, 2));
    }
}

TEST("bit - bit_rotate_right shifts with wraparound")
{
    SECTION("u8 rotations")
    {
        // 0b10110010 rotated right by 2 => 0b10101100
        CHECK(cc::bit_rotate_right(u8(0b10110010), 2) == u8(0b10101100));

        // 0b00000001 rotated right by 1 => 0b10000000
        CHECK(cc::bit_rotate_right(u8(0b00000001), 1) == u8(0b10000000));

        // 0b11110000 rotated right by 4 => 0b00001111
        CHECK(cc::bit_rotate_right(u8(0b11110000), 4) == u8(0b00001111));

        // rotate by 0
        CHECK(cc::bit_rotate_right(u8(0b10101010), 0) == u8(0b10101010));

        // full rotation
        CHECK(cc::bit_rotate_right(u8(0b10101010), 8) == u8(0b10101010));

        // rotate by more than width
        CHECK(cc::bit_rotate_right(u8(0b10110010), 10) == u8(0b10101100)); // same as rotate by 2
    }

    SECTION("u16 rotations")
    {
        CHECK(cc::bit_rotate_right(u16(0x0001), 1) == u16(0x8000));
        CHECK(cc::bit_rotate_right(u16(0x00FF), 8) == u16(0xFF00));
        CHECK(cc::bit_rotate_right(u16(0x1234), 4) == u16(0x4123));
    }

    SECTION("u32 rotations")
    {
        CHECK(cc::bit_rotate_right(u32(0x00000001), 1) == u32(0x80000000));
        CHECK(cc::bit_rotate_right(u32(0x000000FF), 8) == u32(0xFF000000));
        CHECK(cc::bit_rotate_right(u32(0x12345678), 8) == u32(0x78123456));
    }

    SECTION("u64 rotations")
    {
        CHECK(cc::bit_rotate_right(u64(0x0000000000000001ull), 1) == u64(0x8000000000000000ull));
        CHECK(cc::bit_rotate_right(u64(0x00000000000000FFull), 8) == u64(0xFF00000000000000ull));
        CHECK(cc::bit_rotate_right(u64(0x123456789ABCDEF0ull), 16) == u64(0xDEF0123456789ABCull));
    }

    SECTION("negative shift performs left rotation")
    {
        u8 const val = 0b10110010;
        CHECK(cc::bit_rotate_right(val, -2) == cc::bit_rotate_left(val, 2));
    }
}

TEST("bit - rotation equivalence")
{
    SECTION("rotate left N == rotate right (width - N)")
    {
        u8 const val = 0b10110010;
        CHECK(cc::bit_rotate_left(val, 3) == cc::bit_rotate_right(val, 5));
        CHECK(cc::bit_rotate_left(val, 1) == cc::bit_rotate_right(val, 7));
    }

    SECTION("rotate by full width returns original")
    {
        CHECK(cc::bit_rotate_left(u8(0xAB), 8) == u8(0xAB));
        CHECK(cc::bit_rotate_left(u16(0x1234), 16) == u16(0x1234));
        CHECK(cc::bit_rotate_left(u32(0xDEADBEEF), 32) == u32(0xDEADBEEF));
        CHECK(cc::bit_rotate_left(u64(0x123456789ABCDEFull), 64) == u64(0x123456789ABCDEFull));
    }
}

// =========================================================================================================
// Bit counting (leading) tests
// =========================================================================================================

TEST("bit - count_leading_zeroes from MSB")
{
    SECTION("u8 patterns")
    {
        CHECK(cc::count_leading_zeroes(u8(0b00000000)) == 8);
        CHECK(cc::count_leading_zeroes(u8(0b00000001)) == 7);
        CHECK(cc::count_leading_zeroes(u8(0b00001000)) == 4);
        CHECK(cc::count_leading_zeroes(u8(0b00011010)) == 3);
        CHECK(cc::count_leading_zeroes(u8(0b01111111)) == 1);
        CHECK(cc::count_leading_zeroes(u8(0b10000000)) == 0);
        CHECK(cc::count_leading_zeroes(u8(0b11111111)) == 0);
    }

    SECTION("u16 patterns")
    {
        CHECK(cc::count_leading_zeroes(u16(0x0000)) == 16);
        CHECK(cc::count_leading_zeroes(u16(0x0001)) == 15);
        CHECK(cc::count_leading_zeroes(u16(0x00FF)) == 8);
        CHECK(cc::count_leading_zeroes(u16(0x0FFF)) == 4);
        CHECK(cc::count_leading_zeroes(u16(0x7FFF)) == 1);
        CHECK(cc::count_leading_zeroes(u16(0x8000)) == 0);
        CHECK(cc::count_leading_zeroes(u16(0xFFFF)) == 0);
    }

    SECTION("u32 patterns")
    {
        CHECK(cc::count_leading_zeroes(u32(0x00000000)) == 32);
        CHECK(cc::count_leading_zeroes(u32(0x00000001)) == 31);
        CHECK(cc::count_leading_zeroes(u32(0x000000FF)) == 24);
        CHECK(cc::count_leading_zeroes(u32(0x0000FFFF)) == 16);
        CHECK(cc::count_leading_zeroes(u32(0x00FFFFFF)) == 8);
        CHECK(cc::count_leading_zeroes(u32(0x7FFFFFFF)) == 1);
        CHECK(cc::count_leading_zeroes(u32(0x80000000)) == 0);
        CHECK(cc::count_leading_zeroes(u32(0xFFFFFFFF)) == 0);
    }

    SECTION("u64 patterns")
    {
        CHECK(cc::count_leading_zeroes(u64(0x0000000000000000ull)) == 64);
        CHECK(cc::count_leading_zeroes(u64(0x0000000000000001ull)) == 63);
        CHECK(cc::count_leading_zeroes(u64(0x00000000FFFFFFFFull)) == 32);
        CHECK(cc::count_leading_zeroes(u64(0x7FFFFFFFFFFFFFFFull)) == 1);
        CHECK(cc::count_leading_zeroes(u64(0x8000000000000000ull)) == 0);
        CHECK(cc::count_leading_zeroes(u64(0xFFFFFFFFFFFFFFFFull)) == 0);
    }
}

TEST("bit - count_leading_ones from MSB")
{
    SECTION("u8 patterns")
    {
        CHECK(cc::count_leading_ones(u8(0b00000000)) == 0);
        CHECK(cc::count_leading_ones(u8(0b10000000)) == 1);
        CHECK(cc::count_leading_ones(u8(0b11000000)) == 2);
        CHECK(cc::count_leading_ones(u8(0b11100101)) == 3);
        CHECK(cc::count_leading_ones(u8(0b11111110)) == 7);
        CHECK(cc::count_leading_ones(u8(0b11111111)) == 8);
    }

    SECTION("u16 patterns")
    {
        CHECK(cc::count_leading_ones(u16(0x0000)) == 0);
        CHECK(cc::count_leading_ones(u16(0x8000)) == 1);
        CHECK(cc::count_leading_ones(u16(0xFF00)) == 8);
        CHECK(cc::count_leading_ones(u16(0xFFF0)) == 12);
        CHECK(cc::count_leading_ones(u16(0xFFFE)) == 15);
        CHECK(cc::count_leading_ones(u16(0xFFFF)) == 16);
    }

    SECTION("u32 patterns")
    {
        CHECK(cc::count_leading_ones(u32(0x00000000)) == 0);
        CHECK(cc::count_leading_ones(u32(0x80000000)) == 1);
        CHECK(cc::count_leading_ones(u32(0xFF000000)) == 8);
        CHECK(cc::count_leading_ones(u32(0xFFFF0000)) == 16);
        CHECK(cc::count_leading_ones(u32(0xFFFFFF00)) == 24);
        CHECK(cc::count_leading_ones(u32(0xFFFFFFFE)) == 31);
        CHECK(cc::count_leading_ones(u32(0xFFFFFFFF)) == 32);
    }

    SECTION("u64 patterns")
    {
        CHECK(cc::count_leading_ones(u64(0x0000000000000000ull)) == 0);
        CHECK(cc::count_leading_ones(u64(0x8000000000000000ull)) == 1);
        CHECK(cc::count_leading_ones(u64(0xFFFFFFFF00000000ull)) == 32);
        CHECK(cc::count_leading_ones(u64(0xFFFFFFFFFFFFFFFEull)) == 63);
        CHECK(cc::count_leading_ones(u64(0xFFFFFFFFFFFFFFFFull)) == 64);
    }
}

TEST("bit - leading count invariants")
{
    SECTION("leading zeroes == leading ones of inverted value")
    {
        u8 const val = 0b00101010;
        CHECK(cc::count_leading_zeroes(val) == cc::count_leading_ones(u8(~val)));

        // Additional test cases
        CHECK(cc::count_leading_zeroes(u8(0b00000001)) == cc::count_leading_ones(u8(~u8(0b00000001))));
        CHECK(cc::count_leading_zeroes(u16(0x00FF)) == cc::count_leading_ones(u16(~u16(0x00FF))));
        CHECK(cc::count_leading_zeroes(u32(0x0000FFFF)) == cc::count_leading_ones(~u32(0x0000FFFF)));
    }

    SECTION("all zeroes => width leading zeroes, zero leading ones")
    {
        CHECK(cc::count_leading_zeroes(u8(0)) == 8);
        CHECK(cc::count_leading_ones(u8(0)) == 0);
        CHECK(cc::count_leading_zeroes(u32(0)) == 32);
        CHECK(cc::count_leading_ones(u32(0)) == 0);
    }

    SECTION("all ones => zero leading zeroes, width leading ones")
    {
        CHECK(cc::count_leading_zeroes(u8(0xFF)) == 0);
        CHECK(cc::count_leading_ones(u8(0xFF)) == 8);
        CHECK(cc::count_leading_zeroes(u32(0xFFFFFFFF)) == 0);
        CHECK(cc::count_leading_ones(u32(0xFFFFFFFF)) == 32);
    }
}

// =========================================================================================================
// Bit counting (trailing) tests
// =========================================================================================================

TEST("bit - count_trailing_zeroes from LSB")
{
    SECTION("u8 patterns")
    {
        CHECK(cc::count_trailing_zeroes(u8(0b00000000)) == 8);
        CHECK(cc::count_trailing_zeroes(u8(0b00000001)) == 0);
        CHECK(cc::count_trailing_zeroes(u8(0b00000010)) == 1);
        CHECK(cc::count_trailing_zeroes(u8(0b00001000)) == 3);
        CHECK(cc::count_trailing_zeroes(u8(0b01011000)) == 3);
        CHECK(cc::count_trailing_zeroes(u8(0b10000000)) == 7);
    }

    SECTION("u16 patterns")
    {
        CHECK(cc::count_trailing_zeroes(u16(0x0000)) == 16);
        CHECK(cc::count_trailing_zeroes(u16(0x0001)) == 0);
        CHECK(cc::count_trailing_zeroes(u16(0x0010)) == 4);
        CHECK(cc::count_trailing_zeroes(u16(0x0100)) == 8);
        CHECK(cc::count_trailing_zeroes(u16(0x1000)) == 12);
        CHECK(cc::count_trailing_zeroes(u16(0x8000)) == 15);
    }

    SECTION("u32 patterns")
    {
        CHECK(cc::count_trailing_zeroes(u32(0x00000000)) == 32);
        CHECK(cc::count_trailing_zeroes(u32(0x00000001)) == 0);
        CHECK(cc::count_trailing_zeroes(u32(0x00000100)) == 8);
        CHECK(cc::count_trailing_zeroes(u32(0x00010000)) == 16);
        CHECK(cc::count_trailing_zeroes(u32(0x01000000)) == 24);
        CHECK(cc::count_trailing_zeroes(u32(0x80000000)) == 31);
    }

    SECTION("u64 patterns")
    {
        CHECK(cc::count_trailing_zeroes(u64(0x0000000000000000ull)) == 64);
        CHECK(cc::count_trailing_zeroes(u64(0x0000000000000001ull)) == 0);
        CHECK(cc::count_trailing_zeroes(u64(0x0000000100000000ull)) == 32);
        CHECK(cc::count_trailing_zeroes(u64(0x8000000000000000ull)) == 63);
    }
}

TEST("bit - count_trailing_ones from LSB")
{
    SECTION("u8 patterns")
    {
        CHECK(cc::count_trailing_ones(u8(0b00000000)) == 0);
        CHECK(cc::count_trailing_ones(u8(0b00000001)) == 1);
        CHECK(cc::count_trailing_ones(u8(0b00000011)) == 2);
        CHECK(cc::count_trailing_ones(u8(0b10100111)) == 3);
        CHECK(cc::count_trailing_ones(u8(0b01111111)) == 7);
        CHECK(cc::count_trailing_ones(u8(0b11111111)) == 8);
    }

    SECTION("u16 patterns")
    {
        CHECK(cc::count_trailing_ones(u16(0x0000)) == 0);
        CHECK(cc::count_trailing_ones(u16(0x0001)) == 1);
        CHECK(cc::count_trailing_ones(u16(0x000F)) == 4);
        CHECK(cc::count_trailing_ones(u16(0x00FF)) == 8);
        CHECK(cc::count_trailing_ones(u16(0x0FFF)) == 12);
        CHECK(cc::count_trailing_ones(u16(0x7FFF)) == 15);
        CHECK(cc::count_trailing_ones(u16(0xFFFF)) == 16);
    }

    SECTION("u32 patterns")
    {
        CHECK(cc::count_trailing_ones(u32(0x00000000)) == 0);
        CHECK(cc::count_trailing_ones(u32(0x00000001)) == 1);
        CHECK(cc::count_trailing_ones(u32(0x000000FF)) == 8);
        CHECK(cc::count_trailing_ones(u32(0x0000FFFF)) == 16);
        CHECK(cc::count_trailing_ones(u32(0x00FFFFFF)) == 24);
        CHECK(cc::count_trailing_ones(u32(0x7FFFFFFF)) == 31);
        CHECK(cc::count_trailing_ones(u32(0xFFFFFFFF)) == 32);
    }

    SECTION("u64 patterns")
    {
        CHECK(cc::count_trailing_ones(u64(0x0000000000000000ull)) == 0);
        CHECK(cc::count_trailing_ones(u64(0x0000000000000001ull)) == 1);
        CHECK(cc::count_trailing_ones(u64(0x00000000FFFFFFFFull)) == 32);
        CHECK(cc::count_trailing_ones(u64(0x7FFFFFFFFFFFFFFFull)) == 63);
        CHECK(cc::count_trailing_ones(u64(0xFFFFFFFFFFFFFFFFull)) == 64);
    }
}

TEST("bit - trailing count invariants")
{
    SECTION("trailing zeroes == trailing ones of inverted value")
    {
        u8 const val = 0b01011000;
        CHECK(cc::count_trailing_zeroes(val) == cc::count_trailing_ones(u8(~val)));

        // Additional test cases
        CHECK(cc::count_trailing_zeroes(u8(0b10000000)) == cc::count_trailing_ones(u8(~u8(0b10000000))));
        CHECK(cc::count_trailing_zeroes(u16(0xFF00)) == cc::count_trailing_ones(u16(~u16(0xFF00))));
        CHECK(cc::count_trailing_zeroes(u32(0xFFFF0000)) == cc::count_trailing_ones(~u32(0xFFFF0000)));
    }

    SECTION("all zeroes => width trailing zeroes, zero trailing ones")
    {
        CHECK(cc::count_trailing_zeroes(u8(0)) == 8);
        CHECK(cc::count_trailing_ones(u8(0)) == 0);
        CHECK(cc::count_trailing_zeroes(u32(0)) == 32);
        CHECK(cc::count_trailing_ones(u32(0)) == 0);
    }

    SECTION("all ones => zero trailing zeroes, width trailing ones")
    {
        CHECK(cc::count_trailing_zeroes(u8(0xFF)) == 0);
        CHECK(cc::count_trailing_ones(u8(0xFF)) == 8);
        CHECK(cc::count_trailing_zeroes(u32(0xFFFFFFFF)) == 0);
        CHECK(cc::count_trailing_ones(u32(0xFFFFFFFF)) == 32);
    }
}

// =========================================================================================================
// Population count tests
// =========================================================================================================

TEST("bit - popcount counts set bits")
{
    SECTION("u8 patterns")
    {
        CHECK(cc::popcount(u8(0b00000000)) == 0);
        CHECK(cc::popcount(u8(0b00000001)) == 1);
        CHECK(cc::popcount(u8(0b00000011)) == 2);
        CHECK(cc::popcount(u8(0b10110010)) == 4);
        CHECK(cc::popcount(u8(0b01010101)) == 4);
        CHECK(cc::popcount(u8(0b10101010)) == 4);
        CHECK(cc::popcount(u8(0b11111110)) == 7);
        CHECK(cc::popcount(u8(0b11111111)) == 8);
    }

    SECTION("u16 patterns")
    {
        CHECK(cc::popcount(u16(0x0000)) == 0);
        CHECK(cc::popcount(u16(0x0001)) == 1);
        CHECK(cc::popcount(u16(0x00FF)) == 8);
        CHECK(cc::popcount(u16(0x5555)) == 8); // alternating bits
        CHECK(cc::popcount(u16(0xAAAA)) == 8); // alternating bits
        CHECK(cc::popcount(u16(0xFF00)) == 8);
        CHECK(cc::popcount(u16(0xFFFF)) == 16);
    }

    SECTION("u32 patterns")
    {
        CHECK(cc::popcount(u32(0x00000000)) == 0);
        CHECK(cc::popcount(u32(0x00000001)) == 1);
        CHECK(cc::popcount(u32(0x000000FF)) == 8);
        CHECK(cc::popcount(u32(0x0000FFFF)) == 16);
        CHECK(cc::popcount(u32(0x55555555)) == 16); // alternating bits
        CHECK(cc::popcount(u32(0xAAAAAAAA)) == 16); // alternating bits
        CHECK(cc::popcount(u32(0xFFFF0000)) == 16);
        CHECK(cc::popcount(u32(0xFFFFFFFF)) == 32);
    }

    SECTION("u64 patterns")
    {
        CHECK(cc::popcount(u64(0x0000000000000000ull)) == 0);
        CHECK(cc::popcount(u64(0x0000000000000001ull)) == 1);
        CHECK(cc::popcount(u64(0x00000000FFFFFFFFull)) == 32);
        CHECK(cc::popcount(u64(0x5555555555555555ull)) == 32); // alternating bits
        CHECK(cc::popcount(u64(0xAAAAAAAAAAAAAAAAull)) == 32); // alternating bits
        CHECK(cc::popcount(u64(0xFFFFFFFF00000000ull)) == 32);
        CHECK(cc::popcount(u64(0xFFFFFFFFFFFFFFFFull)) == 64);
    }

    SECTION("sparse bit patterns")
    {
        CHECK(cc::popcount(u32(0x80000001)) == 2);
        CHECK(cc::popcount(u32(0x80808080)) == 4);
        CHECK(cc::popcount(u32(0x01010101)) == 4);
        CHECK(cc::popcount(u64(0x8000000000000001ull)) == 2);
    }

    SECTION("popcount of inverted value")
    {
        u8 const val = 0b10110010;
        CHECK(cc::popcount(val) + cc::popcount(u8(~val)) == 8);

        u32 const val32 = 0x12345678;
        CHECK(cc::popcount(val32) + cc::popcount(~val32) == 32);
    }
}

// =========================================================================================================
// Cross-function relationships
// =========================================================================================================

TEST("bit - relationship between bit_width and count_leading_zeroes")
{
    SECTION("bit_width == width - count_leading_zeroes (for non-zero)")
    {
        CHECK(cc::bit_width(u8(0b00001010)) == 8 - cc::count_leading_zeroes(u8(0b00001010)));
        CHECK(cc::bit_width(u8(0b10000000)) == 8 - cc::count_leading_zeroes(u8(0b10000000)));
        CHECK(cc::bit_width(u16(0x0FFF)) == 16 - cc::count_leading_zeroes(u16(0x0FFF)));
        CHECK(cc::bit_width(u32(0x12345678)) == 32 - cc::count_leading_zeroes(u32(0x12345678)));
    }

    SECTION("zero is special case")
    {
        CHECK(cc::bit_width(u8(0)) == 0);
        CHECK(cc::count_leading_zeroes(u8(0)) == 8);
    }
}

TEST("bit - power of two relationships")
{
    SECTION("bit_ceil of power of two is unchanged")
    {
        CHECK(cc::bit_ceil(u8(1)) == 1);
        CHECK(cc::bit_ceil(u8(2)) == 2);
        CHECK(cc::bit_ceil(u8(4)) == 4);
        CHECK(cc::bit_ceil(u8(8)) == 8);
        CHECK(cc::bit_ceil(u32(1024)) == 1024);
    }

    SECTION("bit_floor of power of two is unchanged")
    {
        CHECK(cc::bit_floor(u8(1)) == 1);
        CHECK(cc::bit_floor(u8(2)) == 2);
        CHECK(cc::bit_floor(u8(4)) == 4);
        CHECK(cc::bit_floor(u8(8)) == 8);
        CHECK(cc::bit_floor(u32(1024)) == 1024);
    }

    SECTION("has_single_bit iff bit_ceil == bit_floor (for non-zero)")
    {
        // NOTE: bit_ceil(129) == 256 BUT this is not representable in u8
        for (u8 i = 1; i <= 128; ++i)
        {
            bool const is_pow2 = cc::has_single_bit(i);
            std::cerr << "i = " << (int)i << std::endl;
            bool const ceil_eq_floor = (cc::bit_ceil(i) == cc::bit_floor(i));
            CHECK(is_pow2 == ceil_eq_floor);
        }
    }

    SECTION("power of two has exactly one bit set")
    {
        CHECK(cc::popcount(u8(1)) == 1);
        CHECK(cc::popcount(u8(2)) == 1);
        CHECK(cc::popcount(u8(4)) == 1);
        CHECK(cc::popcount(u8(8)) == 1);
        CHECK(cc::popcount(u8(16)) == 1);
        CHECK(cc::popcount(u32(1u << 20)) == 1);
    }
}

// =========================================================================================================
// Atomic operations tests
// =========================================================================================================

TEST("bit - atomic_add performs atomic addition")
{
    SECTION("u32 addition")
    {
        u32 counter = 0;
        u32 old_val = cc::atomic_add(counter, u32(5));
        CHECK(old_val == 0);
        CHECK(counter == 5);

        old_val = cc::atomic_add(counter, u32(3));
        CHECK(old_val == 5);
        CHECK(counter == 8);
    }

    SECTION("u64 addition")
    {
        u64 counter = 100;
        u64 old_val = cc::atomic_add(counter, u64(25));
        CHECK(old_val == 100);
        CHECK(counter == 125);
    }

    SECTION("i32 addition")
    {
        i32 counter = 10;
        i32 old_val = cc::atomic_add(counter, i32(-5));
        CHECK(old_val == 10);
        CHECK(counter == 5);
    }

    SECTION("sequential additions")
    {
        u32 value = 0;
        for (u32 i = 0; i < 10; ++i)
        {
            cc::atomic_add(value, u32(1));
        }
        CHECK(value == 10);
    }
}

TEST("bit - atomic_sub performs atomic subtraction")
{
    SECTION("u32 subtraction")
    {
        u32 counter = 10;
        u32 old_val = cc::atomic_sub(counter, u32(3));
        CHECK(old_val == 10);
        CHECK(counter == 7);

        old_val = cc::atomic_sub(counter, u32(2));
        CHECK(old_val == 7);
        CHECK(counter == 5);
    }

    SECTION("u64 subtraction")
    {
        u64 counter = 1000;
        u64 old_val = cc::atomic_sub(counter, u64(250));
        CHECK(old_val == 1000);
        CHECK(counter == 750);
    }

    SECTION("i32 subtraction")
    {
        i32 counter = 5;
        i32 old_val = cc::atomic_sub(counter, i32(-10));
        CHECK(old_val == 5);
        CHECK(counter == 15);
    }

    SECTION("sequential subtractions")
    {
        u32 value = 20;
        for (u32 i = 0; i < 5; ++i)
        {
            cc::atomic_sub(value, u32(2));
        }
        CHECK(value == 10);
    }
}

TEST("bit - atomic_and performs atomic bitwise AND")
{
    SECTION("u32 AND operations")
    {
        u32 flags = 0b1111;
        u32 old_val = cc::atomic_and(flags, u32(0b1100));
        CHECK(old_val == 0b1111);
        CHECK(flags == 0b1100);

        old_val = cc::atomic_and(flags, u32(0b1010));
        CHECK(old_val == 0b1100);
        CHECK(flags == 0b1000);
    }

    SECTION("u64 AND operations")
    {
        u64 flags = 0xFFFFFFFFFFFFFFFFull;
        u64 old_val = cc::atomic_and(flags, u64(0x00000000FFFFFFFFull));
        CHECK(old_val == 0xFFFFFFFFFFFFFFFFull);
        CHECK(flags == 0x00000000FFFFFFFFull);
    }

    SECTION("clearing specific bits")
    {
        u32 value = 0b11111111;
        cc::atomic_and(value, u32(0b11110000)); // clear lower 4 bits
        CHECK(value == 0b11110000);
    }

    SECTION("sequential AND operations")
    {
        u32 value = 0xFF;
        cc::atomic_and(value, u32(0xF0));
        cc::atomic_and(value, u32(0xC0));
        cc::atomic_and(value, u32(0x80));
        CHECK(value == 0x80);
    }
}

TEST("bit - atomic_or performs atomic bitwise OR")
{
    SECTION("u32 OR operations")
    {
        u32 flags = 0b0011;
        u32 old_val = cc::atomic_or(flags, u32(0b1100));
        CHECK(old_val == 0b0011);
        CHECK(flags == 0b1111);

        flags = 0b0000;
        old_val = cc::atomic_or(flags, u32(0b0101));
        CHECK(old_val == 0b0000);
        CHECK(flags == 0b0101);
    }

    SECTION("u64 OR operations")
    {
        u64 flags = 0x00000000FFFFFFFFull;
        u64 old_val = cc::atomic_or(flags, u64(0xFFFFFFFF00000000ull));
        CHECK(old_val == 0x00000000FFFFFFFFull);
        CHECK(flags == 0xFFFFFFFFFFFFFFFFull);
    }

    SECTION("setting specific bits")
    {
        u32 value = 0b00000000;
        cc::atomic_or(value, u32(0b00001111)); // set lower 4 bits
        CHECK(value == 0b00001111);
    }

    SECTION("sequential OR operations")
    {
        u32 value = 0x00;
        cc::atomic_or(value, u32(0x01));
        cc::atomic_or(value, u32(0x02));
        cc::atomic_or(value, u32(0x04));
        cc::atomic_or(value, u32(0x08));
        CHECK(value == 0x0F);
    }
}

TEST("bit - atomic_xor performs atomic bitwise XOR")
{
    SECTION("u32 XOR operations")
    {
        u32 flags = 0b0011;
        u32 old_val = cc::atomic_xor(flags, u32(0b1100));
        CHECK(old_val == 0b0011);
        CHECK(flags == 0b1111);

        old_val = cc::atomic_xor(flags, u32(0b1111));
        CHECK(old_val == 0b1111);
        CHECK(flags == 0b0000);
    }

    SECTION("u64 XOR operations")
    {
        u64 flags = 0xAAAAAAAAAAAAAAAAull;
        u64 old_val = cc::atomic_xor(flags, u64(0x5555555555555555ull));
        CHECK(old_val == 0xAAAAAAAAAAAAAAAAull);
        CHECK(flags == 0xFFFFFFFFFFFFFFFFull);
    }

    SECTION("toggling bits")
    {
        u32 value = 0b10101010;
        cc::atomic_xor(value, u32(0b11111111)); // flip all bits
        CHECK(value == 0b01010101);
        cc::atomic_xor(value, u32(0b11111111)); // flip back
        CHECK(value == 0b10101010);
    }

    SECTION("XOR with zero is identity")
    {
        u32 value = 0xDEADBEEF;
        cc::atomic_xor(value, u32(0));
        CHECK(value == 0xDEADBEEF);
    }

    SECTION("XOR with self gives zero")
    {
        u32 value = 0xDEADBEEF;
        u32 original = value;
        cc::atomic_xor(value, original);
        CHECK(value == 0);
    }
}

TEST("bit - atomic operations return old values")
{
    SECTION("return values are pre-operation values")
    {
        u32 value = 10;

        u32 old = cc::atomic_add(value, u32(5));
        CHECK(old == 10);   // old value before add
        CHECK(value == 15); // new value after add

        old = cc::atomic_sub(value, u32(3));
        CHECK(old == 15);   // old value before sub
        CHECK(value == 12); // new value after sub

        old = cc::atomic_and(value, u32(0x0F));
        CHECK(old == 12);   // old value before and
        CHECK(value == 12); // new value after and (12 & 15 = 12)

        old = cc::atomic_or(value, u32(0x10));
        CHECK(old == 12);   // old value before or
        CHECK(value == 28); // new value after or (12 | 16 = 28)

        old = cc::atomic_xor(value, u32(0xFF));
        CHECK(old == 28);    // old value before xor
        CHECK(value == 227); // new value after xor (28 ^ 255 = 227)
    }
}
