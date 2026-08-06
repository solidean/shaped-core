#pragma once

#include <bit>

// Bit manipulation: the <bit> operations re-exported into cc, plus rotations and bit counts spelled out under our own names.
//
// Atomic read-modify-write on a plain lvalue (atomic_add / _sub / _and / _or / _xor) lives in clean-core/thread/atomic.hh, next to cc::atomic.

namespace cc
{
/// Reinterpret one type's object representation as another's.
/// Type punning that respects strict aliasing.
using std::bit_cast;

/// True iff value is an integral power of 2.
/// has_single_bit(0) is false.
using std::has_single_bit;

/// Smallest integral power of 2 not less than value, so bit_ceil(0) is 1.
/// UB if that power is not representable in the type — for u8 that means any value above 128.
using std::bit_ceil;

/// Largest integral power of 2 not greater than value.
/// bit_floor(0) is 0.
using std::bit_floor;

/// Smallest number of bits needed to represent value.
/// bit_width(0) is 0.
using std::bit_width;

/// Bitwise left-rotation by shift positions, wrapping around.
/// A negative shift rotates right instead.
/// T must be an unsigned integer type, here and for every count and popcount below.
template <class T>
[[nodiscard]] constexpr T bit_rotate_left(T value, int shift) noexcept
{
    return std::rotl(value, shift);
}

/// Bitwise right-rotation by shift positions, wrapping around.
/// A negative shift rotates left instead.
template <class T>
[[nodiscard]] constexpr T bit_rotate_right(T value, int shift) noexcept
{
    return std::rotr(value, shift);
}

/// Number of consecutive 0 bits counting down from the most significant bit.
template <class T>
[[nodiscard]] constexpr int count_leading_zeroes(T value) noexcept
{
    return std::countl_zero(value);
}

/// Number of consecutive 1 bits counting down from the most significant bit.
template <class T>
[[nodiscard]] constexpr int count_leading_ones(T value) noexcept
{
    return std::countl_one(value);
}

/// Number of consecutive 0 bits counting up from the least significant bit.
template <class T>
[[nodiscard]] constexpr int count_trailing_zeroes(T value) noexcept
{
    return std::countr_zero(value);
}

/// Number of consecutive 1 bits counting up from the least significant bit.
template <class T>
[[nodiscard]] constexpr int count_trailing_ones(T value) noexcept
{
    return std::countr_one(value);
}

/// Number of 1 bits in an unsigned integer, i.e. its Hamming weight.
using std::popcount;

} // namespace cc
