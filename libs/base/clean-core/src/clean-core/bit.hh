#pragma once

#include <atomic>
#include <bit>

// =========================================================================================================
// Bit manipulation functions
// =========================================================================================================
//
// Bit representation:
//   bit_cast<To>(from)                      - reinterpret object representation as another type
//
// Power of two operations:
//   has_single_bit(value)                   - check if value is integral power of 2
//   bit_ceil(value)                         - smallest power of 2 not less than value
//   bit_floor(value)                        - largest power of 2 not greater than value
//   bit_width(value)                        - smallest number of bits to represent value
//
// Bit rotation:
//   bit_rotate_left(value, shift)           - bitwise left-rotation
//   bit_rotate_right(value, shift)          - bitwise right-rotation
//
// Bit counting (leading):
//   count_leading_zeroes(value)             - count consecutive 0 bits from most significant bit
//   count_leading_ones(value)               - count consecutive 1 bits from most significant bit
//
// Bit counting (trailing):
//   count_trailing_zeroes(value)            - count consecutive 0 bits from least significant bit
//   count_trailing_ones(value)              - count consecutive 1 bits from least significant bit
//
// Population count:
//   popcount(value)                         - count number of 1 bits in unsigned integer
//
// Atomic operations:
//   atomic_add(value, rhs)                  - atomically add rhs to value, return old value
//   atomic_sub(value, rhs)                  - atomically subtract rhs from value, return old value
//   atomic_and(value, rhs)                  - atomically AND rhs with value, return old value
//   atomic_or(value, rhs)                   - atomically OR rhs with value, return old value
//   atomic_xor(value, rhs)                  - atomically XOR rhs with value, return old value
//

namespace cc
{
// =========================================================================================================
// Bit representation
// =========================================================================================================

/// Reinterpret the object representation of one type as that of another
/// Provides safe type punning that respects strict aliasing rules
/// Usage:
///   float f = 3.14f;
///   uint32_t bits = cc::bit_cast<uint32_t>(f);  // get bit representation
///   float restored = cc::bit_cast<float>(bits);  // restore from bits
using std::bit_cast;

// =========================================================================================================
// Power of two operations
// =========================================================================================================

/// Checks if a number is an integral power of 2
/// Returns true if value is a power of 2, false otherwise
/// Usage:
///   bool ok = cc::has_single_bit(8u);    // true
///   bool bad = cc::has_single_bit(10u);  // false
///   // has_single_bit(0) == false
///   // has_single_bit(1) == true
using std::has_single_bit;

/// Finds the smallest integral power of 2 not less than the given value
/// Returns the smallest power of 2 that is >= value (rounds up)
/// WARNING: Undefined behavior if the result is not representable in the type.
///          For uint8_t: UB if value > 128 (result would be 256)
///          For uint16_t: UB if value > 32768 (result would be 65536)
///          For uint32_t: UB if value > 2147483648 (result would be 4294967296)
/// Usage:
///   auto result = cc::bit_ceil(5u);   // 8
///   auto result = cc::bit_ceil(16u);  // 16
///   auto result = cc::bit_ceil(0u);   // 1
using std::bit_ceil;

/// Finds the largest integral power of 2 not greater than the given value
/// Returns the largest power of 2 that is <= value (rounds down)
/// Usage:
///   auto result = cc::bit_floor(5u);   // 4
///   auto result = cc::bit_floor(16u);  // 16
///   auto result = cc::bit_floor(0u);   // 0
using std::bit_floor;

/// Finds the smallest number of bits needed to represent the given value
/// Returns the number of bits required (0 for value 0, 1 for value 1, etc.)
/// Usage:
///   auto bits = cc::bit_width(0u);    // 0
///   auto bits = cc::bit_width(1u);    // 1
///   auto bits = cc::bit_width(5u);    // 3 (0b101 requires 3 bits)
///   auto bits = cc::bit_width(255u);  // 8
using std::bit_width;

// =========================================================================================================
// Bit rotation
// =========================================================================================================

/// Computes the result of bitwise left-rotation
/// Rotates bits to the left by shift positions (with wrap-around)
/// Usage:
///   u8 x = 0b10110010;
///   auto result = cc::bit_rotate_left(x, 2);  // 0b11001010
///   // Negative shift performs right rotation
template <class T>
[[nodiscard]] constexpr T bit_rotate_left(T value, int shift) noexcept
{
    return std::rotl(value, shift);
}

/// Computes the result of bitwise right-rotation
/// Rotates bits to the right by shift positions (with wrap-around)
/// Usage:
///   u8 x = 0b10110010;
///   auto result = cc::bit_rotate_right(x, 2);  // 0b10101100
///   // Negative shift performs left rotation
template <class T>
[[nodiscard]] constexpr T bit_rotate_right(T value, int shift) noexcept
{
    return std::rotr(value, shift);
}

// =========================================================================================================
// Bit counting (leading)
// =========================================================================================================

/// Counts the number of consecutive 0 bits, starting from the most significant bit
/// Returns the number of leading zero bits
/// Usage:
///   auto count = cc::count_leading_zeroes(u8(0b00011010));  // 3
///   auto count = cc::count_leading_zeroes(u8(0));           // 8 (all bits are 0)
///   auto count = cc::count_leading_zeroes(u8(0xFF));        // 0
template <class T>
[[nodiscard]] constexpr int count_leading_zeroes(T value) noexcept
{
    return std::countl_zero(value);
}

/// Counts the number of consecutive 1 bits, starting from the most significant bit
/// Returns the number of leading one bits
/// Usage:
///   auto count = cc::count_leading_ones(u8(0b11100101));  // 3
///   auto count = cc::count_leading_ones(u8(0xFF));        // 8 (all bits are 1)
///   auto count = cc::count_leading_ones(u8(0));           // 0
template <class T>
[[nodiscard]] constexpr int count_leading_ones(T value) noexcept
{
    return std::countl_one(value);
}

// =========================================================================================================
// Bit counting (trailing)
// =========================================================================================================

/// Counts the number of consecutive 0 bits, starting from the least significant bit
/// Returns the number of trailing zero bits
/// Usage:
///   auto count = cc::count_trailing_zeroes(u8(0b01011000));  // 3
///   auto count = cc::count_trailing_zeroes(u8(0));           // 8 (all bits are 0)
///   auto count = cc::count_trailing_zeroes(u8(1));           // 0
template <class T>
[[nodiscard]] constexpr int count_trailing_zeroes(T value) noexcept
{
    return std::countr_zero(value);
}

/// Counts the number of consecutive 1 bits, starting from the least significant bit
/// Returns the number of trailing one bits
/// Usage:
///   auto count = cc::count_trailing_ones(u8(0b10100111));  // 3
///   auto count = cc::count_trailing_ones(u8(0xFF));        // 8 (all bits are 1)
///   auto count = cc::count_trailing_ones(u8(0));           // 0
template <class T>
[[nodiscard]] constexpr int count_trailing_ones(T value) noexcept
{
    return std::countr_one(value);
}

// =========================================================================================================
// Population count
// =========================================================================================================

/// Counts the number of 1 bits in an unsigned integer
/// Returns the total number of set bits (Hamming weight)
/// Usage:
///   auto count = cc::popcount(u8(0b10110010));  // 4
///   auto count = cc::popcount(u8(0));           // 0
///   auto count = cc::popcount(u8(0xFF));        // 8
using std::popcount;

// =========================================================================================================
// Atomic operations
// =========================================================================================================

/// Atomically adds a value and returns the old value
/// Creates a temporary atomic_ref and performs fetch_add
/// Usage:
///   int counter = 0;
///   int old_val = cc::atomic_add(counter, 1);  // counter is now 1, old_val is 0
template <class T>
T atomic_add(T& v, T rhs) noexcept
{
    return std::atomic_ref<T>(v).fetch_add(rhs);
}

/// Atomically subtracts a value and returns the old value
/// Creates a temporary atomic_ref and performs fetch_sub
/// Usage:
///   int counter = 10;
///   int old_val = cc::atomic_sub(counter, 3);  // counter is now 7, old_val is 10
template <class T>
T atomic_sub(T& v, T rhs) noexcept
{
    return std::atomic_ref<T>(v).fetch_sub(rhs);
}

/// Atomically performs bitwise AND and returns the old value
/// Creates a temporary atomic_ref and performs fetch_and
/// Usage:
///   int flags = 0b1111;
///   int old_val = cc::atomic_and(flags, 0b1100);  // flags is now 0b1100, old_val is 0b1111
template <class T>
T atomic_and(T& v, T rhs) noexcept
{
    return std::atomic_ref<T>(v).fetch_and(rhs);
}

/// Atomically performs bitwise OR and returns the old value
/// Creates a temporary atomic_ref and performs fetch_or
/// Usage:
///   int flags = 0b0011;
///   int old_val = cc::atomic_or(flags, 0b1100);  // flags is now 0b1111, old_val is 0b0011
template <class T>
T atomic_or(T& v, T rhs) noexcept
{
    return std::atomic_ref<T>(v).fetch_or(rhs);
}

/// Atomically performs bitwise XOR and returns the old value
/// Creates a temporary atomic_ref and performs fetch_xor
/// Usage:
///   int flags = 0b0011;
///   int old_val = cc::atomic_xor(flags, 0b1100);  // flags is now 0b1111, old_val is 0b0011
template <class T>
T atomic_xor(T& v, T rhs) noexcept
{
    return std::atomic_ref<T>(v).fetch_xor(rhs);
}

} // namespace cc
