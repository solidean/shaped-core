#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/math/bit.hh>
#include <typed-geometry/scalar/fwd.hh>

// The binary16 conversions behind tg::half_float, and nothing else.
// Every path rounds to nearest with ties to even, overflows to infinity, and turns a NaN into a quiet NaN that keeps its
// sign and the top bits of its payload — which is what F16C and AArch64's fcvt do, so all paths agree bit for bit.
// Users include scalar/half_float.hh.

// The hardware is used only where the build target already guarantees it, never after a run-time CPU check:
// a function compiled for F16C cannot be inlined into a caller compiled for baseline x64.
// clang-cl buckets as CC_COMPILER_CLANG, so the MSVC arm is cl.exe, which enables F16C with /arch:AVX2.
#if defined(CC_ARCH_X64) && (defined(__F16C__) || (defined(CC_COMPILER_MSVC) && defined(__AVX2__)))
#define TG_HALF_FLOAT_F16C 1
#include <immintrin.h>
#else
#define TG_HALF_FLOAT_F16C 0
#endif

// AArch64 converts between half and single in its base instruction set, reached through the __fp16 storage type.
// cl.exe has no half type on ARM64, so it takes the portable path.
#if defined(CC_ARCH_ARM64) && (defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC))
#define TG_HALF_FLOAT_ARM64 1
#else
#define TG_HALF_FLOAT_ARM64 0
#endif

namespace tg::impl
{
/// Widens binary16 bits to the f32 of the same value, exactly; a signaling NaN comes back quiet.
[[nodiscard]] constexpr f32 half_bits_to_f32_portable(u16 h)
{
    constexpr u32 shifted_exponent = u32(0x7c00) << 13;
    auto bits = u32(h & 0x7fff) << 13;
    auto const exponent = bits & shifted_exponent;
    bits += u32(127 - 15) << 23;

    if (exponent == shifted_exponent) // infinity or NaN: the exponent field is all ones in both formats
    {
        bits += u32(128 - 16) << 23;
        if ((h & 0x03ff) != 0)
            bits |= u32(1) << 22;
    }
    else if (exponent == 0) // zero or subnormal: renormalize by letting an exact float subtraction do it
    {
        bits += u32(1) << 23;
        bits = cc::bit_cast<u32>(cc::bit_cast<f32>(bits) - cc::bit_cast<f32>(u32(113) << 23));
    }

    return cc::bit_cast<f32>(bits | (u32(h & 0x8000) << 16));
}

/// Narrows an f32 to binary16 bits, correctly rounded.
/// The subnormal branch rounds by a float addition, so it relies on the default round-to-nearest mode.
[[nodiscard]] constexpr u16 f32_to_half_bits_portable(f32 value)
{
    auto bits = cc::bit_cast<u32>(value);
    auto const sign = u16((bits >> 16) & 0x8000);
    bits &= 0x7fff'ffff;

    u16 magnitude = 0;
    if (bits >= u32(127 + 16) << 23) // 2^16 and above: infinity, or a NaN
    {
        auto const is_nan = bits > u32(255) << 23;
        magnitude = is_nan ? u16(0x7e00 | ((bits >> 13) & 0x03ff)) : u16(0x7c00);
    }
    else if (bits < u32(127 - 14) << 23) // below 2^-14: a subnormal or zero
    {
        // Adding 0.5 leaves an f32 whose last place is 2^-24, the half subnormal step, so the addition rounds exactly as the narrowing must.
        constexpr u32 half = u32(126) << 23;
        auto const sum = cc::bit_cast<u32>(cc::bit_cast<f32>(bits) + cc::bit_cast<f32>(half));
        magnitude = u16(sum - half);
    }
    else
    {
        auto const odd = (bits >> 13) & 1;
        bits -= u32(127 - 15) << 23; // rebias; a carry out of the fraction rounds up into the exponent, reaching infinity at 65520
        bits += 0x0fff + odd;
        magnitude = u16(bits >> 13);
    }

    return u16(sign | magnitude);
}

/// Narrows an f64 to binary16 bits in one rounding; going through f32 would round twice.
[[nodiscard]] constexpr u16 f64_to_half_bits_portable(f64 value)
{
    auto bits = cc::bit_cast<u64>(value);
    auto const sign = u16((bits >> 48) & 0x8000);
    bits &= 0x7fff'ffff'ffff'ffff;

    u16 magnitude = 0;
    if (bits >= u64(1023 + 16) << 52)
    {
        auto const is_nan = bits > u64(2047) << 52;
        magnitude = is_nan ? u16(0x7e00 | ((bits >> 42) & 0x03ff)) : u16(0x7c00);
    }
    else if (bits < u64(1023 - 14) << 52)
    {
        // 2^28 is the f64 whose last place is 2^-24, which plays the part 0.5 plays for an f32.
        constexpr u64 magic = u64(1023 + 28) << 52;
        auto const sum = cc::bit_cast<u64>(cc::bit_cast<f64>(bits) + cc::bit_cast<f64>(magic));
        magnitude = u16(sum - magic);
    }
    else
    {
        auto const odd = (bits >> 42) & 1;
        bits -= u64(1023 - 15) << 52;
        bits += (u64(1) << 41) - 1 + odd;
        magnitude = u16(bits >> 42);
    }

    return u16(sign | magnitude);
}

[[nodiscard]] constexpr f32 half_bits_to_f32(u16 h)
{
    if consteval
    {
        return tg::impl::half_bits_to_f32_portable(h);
    }
    else
    {
#if TG_HALF_FLOAT_F16C
        return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(int(h))));
#elif TG_HALF_FLOAT_ARM64
        return f32(cc::bit_cast<__fp16>(h));
#else
        return tg::impl::half_bits_to_f32_portable(h);
#endif
    }
}

[[nodiscard]] constexpr u16 f32_to_half_bits(f32 value)
{
    if consteval
    {
        return tg::impl::f32_to_half_bits_portable(value);
    }
    else
    {
#if TG_HALF_FLOAT_F16C
        return u16(_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(value), _MM_FROUND_TO_NEAREST_INT)));
#elif TG_HALF_FLOAT_ARM64
        return cc::bit_cast<u16>(__fp16(value));
#else
        return tg::impl::f32_to_half_bits_portable(value);
#endif
    }
}
} // namespace tg::impl
