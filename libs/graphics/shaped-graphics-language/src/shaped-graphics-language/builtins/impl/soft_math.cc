#include "soft_math.hh"

#include <clean-core/math/bit.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
constexpr f64 k_pi = 3.141592653589793238462643383279502884;
constexpr f64 k_half_pi = k_pi / 2;
constexpr f64 k_ln2 = 0.693147180559945309417232121458176568;
constexpr f64 k_sqrt2 = 1.41421356237309504880168872420969808;
// pi / 2 split in three, so that `x - n * pi / 2` loses nothing for the n a float argument can have
constexpr f64 k_half_pi_1 = 1.57079632673412561417e+00;
constexpr f64 k_half_pi_2 = 6.07710050650619224932e-11;
constexpr f64 k_half_pi_3 = 2.02226624879595063154e-21;
/// 2^52: from here on every f64 is an integer.
constexpr f64 k_integral = 4503599627370496.0;

bool is_nan(f64 x)
{
    return x != x;
}
bool is_finite(f64 x)
{
    return x - x == 0.0;
}
f64 infinity()
{
    return cc::bit_cast<f64>(u64(0x7ff0000000000000ull));
}
f64 quiet_nan()
{
    return cc::bit_cast<f64>(u64(0x7ff8000000000000ull));
}
f64 absolute(f64 x)
{
    return x < 0.0 ? -x : x;
}

/// `x * 2^k` for any `k` a result can need, in steps that stay inside the exponent range.
f64 scaled(f64 x, int k)
{
    while (k > 1000)
    {
        x *= cc::bit_cast<f64>(u64(1000 + 1023) << 52);
        k -= 1000;
    }
    while (k < -1000)
    {
        x *= cc::bit_cast<f64>(u64(-1000 + 1023) << 52);
        k += 1000;
    }
    return x * cc::bit_cast<f64>(u64(k + 1023) << 52);
}

/// sin and cos of a reduced |r| <= pi / 4, by their series.
f64 sin_series(f64 r)
{
    auto const r2 = r * r;
    auto term = r;
    auto sum = r;
    for (auto n = 1; n < 12; ++n)
    {
        term *= -r2 / f64((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
f64 cos_series(f64 r)
{
    auto const r2 = r * r;
    auto term = 1.0;
    auto sum = 1.0;
    for (auto n = 1; n < 12; ++n)
    {
        term *= -r2 / f64((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

/// sin of x shifted by `quarter_turns` quarter turns: 0 is sin, 1 is cos.
f64 sin_by_quadrant(f64 x, int quarter_turns)
{
    if (!is_finite(x))
        return quiet_nan();
    auto const n = impl::soft_round(x / k_half_pi);
    auto const r = ((x - n * k_half_pi_1) - n * k_half_pi_2) - n * k_half_pi_3;
    // n mod 4, which the low bits of a huge n no longer hold exactly; that far out nothing is exact anyway
    auto const quarter = n - 4.0 * impl::soft_floor(n / 4.0);
    switch ((int(quarter) + quarter_turns) & 3)
    {
    case 0:
        return sin_series(r);
    case 1:
        return cos_series(r);
    case 2:
        return -sin_series(r);
    default:
        return -cos_series(r);
    }
}
} // namespace

f64 impl::soft_trunc(f64 x)
{
    if (!(absolute(x) < k_integral))
        return x;
    auto const t = f64(i64(x));
    return t == 0.0 && x < 0.0 ? -0.0 : t;
}

f64 impl::soft_floor(f64 x)
{
    auto const t = soft_trunc(x);
    return t > x ? t - 1.0 : t;
}

f64 impl::soft_round(f64 x)
{
    if (!(absolute(x) < k_integral))
        return x;
    auto const below = soft_floor(x);
    auto const rest = x - below;
    if (rest < 0.5)
        return below == 0.0 && x < 0.0 ? -0.0 : below;
    if (rest > 0.5)
        return below + 1.0;
    // a tie goes to the even neighbour
    return soft_floor(below / 2.0) * 2.0 == below ? below : below + 1.0;
}

f64 impl::soft_sqrt(f64 x)
{
    if (is_nan(x) || x < 0.0)
        return quiet_nan();
    if (x == 0.0 || !is_finite(x))
        return x;
    // halving the exponent is a guess within a factor of two, which Newton's method doubles the digits of per step
    auto guess = cc::bit_cast<f64>((cc::bit_cast<u64>(x) >> 1) + (u64(1023) << 51));
    for (auto i = 0; i < 8; ++i)
        guess = 0.5 * (guess + x / guess);
    return guess;
}

f64 impl::soft_exp(f64 x)
{
    if (is_nan(x))
        return x;
    if (x > 709.8)
        return infinity();
    if (x < -745.2)
        return 0.0;
    // e^x = 2^k * e^r, with |r| <= ln2 / 2
    auto const k = soft_round(x / k_ln2);
    auto const r = x - k * k_ln2;
    auto term = 1.0;
    auto sum = 1.0;
    for (auto n = 1; n < 24; ++n)
    {
        term *= r / f64(n);
        sum += term;
    }
    return scaled(sum, int(k));
}

f64 impl::soft_log(f64 x)
{
    if (is_nan(x) || x < 0.0)
        return quiet_nan();
    if (x == 0.0)
        return -infinity();
    if (!is_finite(x))
        return x;

    // x = m * 2^e with m in [sqrt(1/2), sqrt(2)), after lifting a subnormal into the normal range
    auto e = 0;
    if (x < cc::bit_cast<f64>(u64(1) << 52))
    {
        x *= cc::bit_cast<f64>(u64(54 + 1023) << 52);
        e -= 54;
    }
    auto const bits = cc::bit_cast<u64>(x);
    e += int((bits >> 52) & 0x7ff) - 1023;
    auto m = cc::bit_cast<f64>((bits & ((u64(1) << 52) - 1)) | (u64(1023) << 52));
    if (m > k_sqrt2)
    {
        m *= 0.5;
        ++e;
    }

    // ln m = 2 atanh s, with s = (m - 1) / (m + 1) and |s| < 0.18
    auto const s = (m - 1.0) / (m + 1.0);
    auto const s2 = s * s;
    auto power = s;
    auto sum = 0.0;
    for (auto n = 0; n < 24; ++n)
    {
        sum += power / f64(2 * n + 1);
        power *= s2;
    }
    return 2.0 * sum + f64(e) * k_ln2;
}

f64 impl::soft_sin(f64 x)
{
    return sin_by_quadrant(x, 0);
}

f64 impl::soft_cos(f64 x)
{
    return sin_by_quadrant(x, 1);
}

f64 impl::soft_atan(f64 x)
{
    if (is_nan(x))
        return x;
    if (x < 0.0)
        return -soft_atan(-x);
    if (x > 1.0)
        return k_half_pi - soft_atan(1.0 / x);
    // atan x = 2 atan(x / (1 + sqrt(1 + x^2))), twice, leaves |x| <= tan(pi / 16), where the series is quick
    auto const once = x / (1.0 + soft_sqrt(1.0 + x * x));
    auto const twice = once / (1.0 + soft_sqrt(1.0 + once * once));
    auto const t2 = twice * twice;
    auto power = twice;
    auto sum = 0.0;
    for (auto n = 0; n < 24; ++n)
    {
        sum += (n % 2 == 0 ? power : -power) / f64(2 * n + 1);
        power *= t2;
    }
    return 4.0 * sum;
}

f64 impl::soft_atan2(f64 y, f64 x)
{
    if (is_nan(x) || is_nan(y))
        return quiet_nan();
    if (x == 0.0)
        return y > 0.0 ? k_half_pi : y < 0.0 ? -k_half_pi : 0.0;
    auto const base = soft_atan(y / x);
    if (x > 0.0)
        return base;
    return y < 0.0 ? base - k_pi : base + k_pi;
}

u32 impl::half_bits_of(f32 x)
{
    auto const bits = cc::bit_cast<u32>(x);
    auto const sign = (bits >> 16) & 0x8000u;
    auto const exponent = int((bits >> 23) & 0xff);
    auto mantissa = bits & 0x7fffffu;

    if (exponent == 0xff)
        return sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u);

    auto const e = exponent - 127 + 15;
    if (e >= 31)
        return sign | 0x7c00u;
    if (e <= 0)
    {
        // a subnormal half, or zero: shift the full mantissa into place and round to even
        if (e < -10)
            return sign;
        mantissa |= 0x800000u;
        auto const shift = u32(14 - e);
        auto const half = mantissa >> shift;
        auto const rest = mantissa & ((1u << shift) - 1);
        auto const midway = 1u << (shift - 1);
        auto const rounded = half + ((rest > midway || (rest == midway && (half & 1u) != 0)) ? 1u : 0u);
        return sign | rounded;
    }

    auto half = sign | (u32(e) << 10) | (mantissa >> 13);
    auto const rest = mantissa & 0x1fffu;
    // a carry out of the mantissa steps the exponent, and out of the largest one into infinity, as rounding should
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0))
        ++half;
    return half;
}

f32 impl::float_of_half_bits(u32 h)
{
    auto const sign = (h & 0x8000u) << 16;
    auto const exponent = (h >> 10) & 0x1fu;
    auto const mantissa = h & 0x3ffu;
    if (exponent == 0x1f)
        return cc::bit_cast<f32>(sign | 0x7f800000u | (mantissa << 13));
    if (exponent == 0)
    {
        // zero or a subnormal half, which every f32 holds exactly
        auto const magnitude = f32(mantissa) * (1.0f / 16777216.0f);
        return sign != 0 ? -magnitude : magnitude;
    }
    return cc::bit_cast<f32>(sign | ((exponent - 15 + 127) << 23) | (mantissa << 13));
}
