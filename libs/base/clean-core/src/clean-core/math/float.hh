#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

// Float classification read from the IEEE-754 bit pattern, so every answer is constexpr and exact under any compiler flag.
// A value is non-finite when its exponent field is all ones: a NaN when the significand is non-zero, an infinity when it is zero.

namespace cc
{
/// Whether x is a NaN, quiet or signaling, of either sign.
[[nodiscard]] constexpr bool is_nan(f32 x) noexcept
{
    return (cc::bit_cast<u32>(x) & 0x7FFF'FFFFu) > 0x7F80'0000u;
}

/// Whether x is a NaN, quiet or signaling, of either sign.
[[nodiscard]] constexpr bool is_nan(f64 x) noexcept
{
    return (cc::bit_cast<u64>(x) & 0x7FFF'FFFF'FFFF'FFFFull) > 0x7FF0'0000'0000'0000ull;
}

/// Whether x is an infinity, of either sign.
[[nodiscard]] constexpr bool is_inf(f32 x) noexcept
{
    return (cc::bit_cast<u32>(x) & 0x7FFF'FFFFu) == 0x7F80'0000u;
}

/// Whether x is an infinity, of either sign.
[[nodiscard]] constexpr bool is_inf(f64 x) noexcept
{
    return (cc::bit_cast<u64>(x) & 0x7FFF'FFFF'FFFF'FFFFull) == 0x7FF0'0000'0000'0000ull;
}

/// Whether x is neither a NaN nor an infinity; zeros and subnormals are finite.
[[nodiscard]] constexpr bool is_finite(f32 x) noexcept
{
    return (cc::bit_cast<u32>(x) & 0x7F80'0000u) != 0x7F80'0000u;
}

/// Whether x is neither a NaN nor an infinity; zeros and subnormals are finite.
[[nodiscard]] constexpr bool is_finite(f64 x) noexcept
{
    return (cc::bit_cast<u64>(x) & 0x7FF0'0000'0000'0000ull) != 0x7FF0'0000'0000'0000ull;
}
} // namespace cc
