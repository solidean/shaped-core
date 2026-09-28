#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/scalar/fwd.hh>
#include <typed-geometry/scalar/impl/half_float_convert.hh>
#include <typed-geometry/scalar/traits.hh>

#include <type_traits>

// The operations tg::half_float computes on its bits, and the one it computes in f32.
// Users include scalar/half_float.hh; this is what its inline bodies and its scalar_traits call.

// Native half arithmetic gives the same bits as the f32 route, so using it is purely a speed choice.
#if TG_HALF_FLOAT_ARM64 && defined(__ARM_FEATURE_FP16_SCALAR_ARITHMETIC)
#define TG_HALF_FLOAT_NATIVE_ARITHMETIC 1
#else
#define TG_HALF_FLOAT_NATIVE_ARITHMETIC 0
#endif

namespace tg::impl
{
/// A builtin integer an f16 constructs from: not bool, not the character types.
template <class I>
concept half_float_integer
    = std::is_integral_v<I> && !std::is_same_v<I, bool> && !std::is_same_v<I, char> && !std::is_same_v<I, char8_t>
   && !std::is_same_v<I, char16_t> && !std::is_same_v<I, char32_t> && !std::is_same_v<I, wchar_t>;

[[nodiscard]] constexpr bool half_bits_are_nan(u16 h)
{
    return (h & 0x7fff) > 0x7c00;
}

/// An integer whose order is the numeric order of the non-NaN halves, with both zeros mapped to 0.
[[nodiscard]] constexpr int half_bits_order_key(u16 h)
{
    auto const magnitude = int(h & 0x7fff);
    return (h & 0x8000) != 0 ? -magnitude : magnitude;
}

enum class half_operation
{
    add,
    subtract,
    multiply,
    divide,
};

template <class T>
[[nodiscard]] constexpr T half_apply(T x, T y, half_operation op)
{
    switch (op)
    {
    case half_operation::add:
        return x + y;
    case half_operation::subtract:
        return x - y;
    case half_operation::multiply:
        return x * y;
    case half_operation::divide:
        return x / y;
    }
    return x;
}

/// One binary16 operation, rounded once: through f32, or natively where the target has half arithmetic.
[[nodiscard]] constexpr u16 half_bits_arithmetic(u16 a, u16 b, half_operation op)
{
#if TG_HALF_FLOAT_NATIVE_ARITHMETIC
    if !consteval
    {
        auto const r = _Float16(tg::impl::half_apply(cc::bit_cast<_Float16>(a), cc::bit_cast<_Float16>(b), op));
        return cc::bit_cast<u16>(r);
    }
#endif
    return tg::impl::f32_to_half_bits(
        tg::impl::half_apply(tg::impl::half_bits_to_f32(a), tg::impl::half_bits_to_f32(b), op));
}

enum class half_rounding
{
    floor,
    ceil,
    nearest_away,
};

/// Rounds to an integral value on the bits, by clearing the fraction bits below the exponent; a NaN comes back quiet.
[[nodiscard]] constexpr u16 half_bits_round(u16 h, half_rounding mode)
{
    auto const sign = u16(h & 0x8000);
    auto const magnitude = u16(h & 0x7fff);
    auto const exponent = int(magnitude >> 10);

    if (exponent == 31)
        return (magnitude & 0x03ff) != 0 ? u16(h | 0x0200) : h;
    if (exponent >= 25 || magnitude == 0) // |x| >= 1024 is integral already, and a zero keeps its sign
        return h;

    if (exponent < 15) // 0 < |x| < 1, subnormals included
    {
        switch (mode)
        {
        case half_rounding::floor:
            return sign != 0 ? u16(0xbc00) : u16(0);
        case half_rounding::ceil:
            return sign != 0 ? u16(0x8000) : u16(0x3c00);
        case half_rounding::nearest_away:
            return exponent == 14 ? u16(sign | 0x3c00) : sign;
        }
    }

    // One unit in the last integral place; adding it to the magnitude carries into the exponent where it must.
    auto const unit = u16(1u << (25 - exponent));
    auto const fraction_mask = u16(unit - 1);
    auto const truncated = u16(h & ~fraction_mask);
    auto const has_fraction = (h & fraction_mask) != 0;

    switch (mode)
    {
    case half_rounding::floor:
        return sign != 0 && has_fraction ? u16(truncated + unit) : truncated;
    case half_rounding::ceil:
        return sign == 0 && has_fraction ? u16(truncated + unit) : truncated;
    case half_rounding::nearest_away:
        return u16((h + (unit >> 1)) & ~fraction_mask);
    }
    return h;
}

/// h * 2^n, saturating to +-infinity or zero; an exponent-field add while both h and the result are normal.
[[nodiscard]] inline u16 half_bits_scale_by_pow2(u16 h, int n)
{
    auto const exponent = int((h >> 10) & 0x1f);
    auto const clamped = n < -64 ? -64 : n > 64 ? 64 : n; // any shift past 64 saturates in every format involved
    auto const scaled = exponent + clamped;
    if (exponent >= 1 && exponent <= 30 && scaled >= 1 && scaled <= 30)
        return u16(int(h) + clamped * 1024);
    return tg::impl::f32_to_half_bits(tg::scalar_traits<f32>::scale_by_pow2(tg::impl::half_bits_to_f32(h), clamped));
}

/// h == significand * 2^exponent with the significand in [1, 2), both as bits.
/// h must be finite and non-zero; a subnormal is normalized.
[[nodiscard]] inline pow2_split<u16> half_bits_split_pow2(u16 h)
{
    CC_ASSERT((h & 0x7c00) != 0x7c00 && (h & 0x7fff) != 0, "split_pow2 needs a finite, non-zero scalar");

    auto const exponent = int((h >> 10) & 0x1f);
    if (exponent != 0)
        return {.significand = u16((h & 0x83ff) | (15 << 10)), .exponent = exponent - 15};

    // a subnormal's significand has at most ten fraction bits, so narrowing it back is exact
    auto const wide = tg::scalar_traits<f32>::split_pow2(tg::impl::half_bits_to_f32(h));
    return {.significand = tg::impl::f32_to_half_bits(wide.significand), .exponent = wide.exponent};
}

/// The value, as the f64 of its shortest decimal digits that read back as the same f16.
/// v must be finite.
[[nodiscard]] f64 half_float_shortest_value(half_float v);
} // namespace tg::impl
