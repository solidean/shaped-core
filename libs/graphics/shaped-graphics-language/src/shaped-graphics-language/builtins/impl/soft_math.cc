#include "soft_math.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/math/wide_arith.hh>

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
bool is_negative(f64 x)
{
    return (cc::bit_cast<u64>(x) >> 63) != 0;
}
/// A zero with the sign of `x`, which is what rounding a value between -1 and 0 gives on a GPU.
f64 signed_zero(f64 x)
{
    return is_negative(x) ? -0.0 : 0.0;
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
    return t == 0.0 ? signed_zero(x) : t;
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
    auto const rounded = rest < 0.5 ? below
                       : rest > 0.5 ? below + 1.0
                       // a tie goes to the even neighbour
                       : soft_floor(below / 2.0) * 2.0 == below ? below
                                                                : below + 1.0;
    return rounded == 0.0 ? signed_zero(x) : rounded;
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

f64 impl::correctly_rounded_sqrt(f64 x)
{
    if (is_nan(x) || x < 0.0)
        return quiet_nan();
    if (x == 0.0 || !is_finite(x))
        return x;

    // x = m * 2^e with m an integer of 53 bits and e even, a subnormal's mantissa shifted up to the same width
    auto const bits = cc::bit_cast<u64>(x);
    auto m = bits & ((u64(1) << 52) - 1);
    auto e = int((bits >> 52) & 0x7ff);
    if (e == 0)
    {
        while ((m >> 52) == 0)
        {
            m <<= 1;
            --e;
        }
        ++e;
    }
    else
        m |= u64(1) << 52;
    e -= 1075;
    if ((e & 1) != 0)
    {
        m <<= 1;
        --e;
    }

    // the root of m * 2^56, a bit pair of it at a time, so the 55-bit result has two bits past the 53 it keeps
    auto root = u64(0);
    auto remainder = u64(0);
    for (auto pair = 54; pair >= 0; --pair)
    {
        auto const low_bit = 2 * pair - 56;
        auto const bits_here = low_bit >= 0 ? (m >> low_bit) & 3u : u64(0);
        remainder = (remainder << 2) | bits_here;
        auto const trial = (root << 2) | 1u;
        root <<= 1;
        if (remainder >= trial)
        {
            remainder -= trial;
            root |= 1u;
        }
    }

    // to nearest, ties to even, though a root of a double is never a tie
    auto const kept = root >> 2;
    auto const guard = (root >> 1) & 1u;
    auto const sticky = (root & 1u) != 0 || remainder != 0;
    auto const rounded = kept + ((guard != 0 && (sticky || (kept & 1u) != 0)) ? 1u : 0u);
    // the root is rounded * 2^(e / 2 - 28 + 2), and rounded has 53 bits, or 54 where rounding carried
    return scaled(f64(rounded), e / 2 - 26);
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
    // every f32 is an f64, so this rounds once
    return half_bits_of(f64(x));
}

u32 impl::half_bits_of(f64 x)
{
    auto const bits = cc::bit_cast<u64>(x);
    auto const sign = u32(bits >> 48) & 0x8000u;
    auto const exponent = int((bits >> 52) & 0x7ff);
    auto mantissa = bits & ((u64(1) << 52) - 1);

    if (exponent == 0x7ff)
        return sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u);

    auto const e = exponent - 1023 + 15;
    if (e >= 31)
        return sign | 0x7c00u;
    if (e <= 0)
    {
        // a subnormal half, or zero: shift the full mantissa into place and round to even
        if (e < -10)
            return sign;
        mantissa |= u64(1) << 52;
        auto const shift = u32(43 - e);
        auto const half = mantissa >> shift;
        auto const rest = mantissa & ((u64(1) << shift) - 1);
        auto const midway = u64(1) << (shift - 1);
        auto const rounded = half + ((rest > midway || (rest == midway && (half & 1u) != 0)) ? 1u : 0u);
        return sign | u32(rounded);
    }

    auto half = sign | (u32(e) << 10) | u32(mantissa >> 42);
    auto const rest = mantissa & ((u64(1) << 42) - 1);
    auto const midway = u64(1) << 41;
    // a carry out of the mantissa steps the exponent, and out of the largest one into infinity, as rounding should
    if (rest > midway || (rest == midway && (half & 1u) != 0))
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

namespace
{
/// The value of a finite f64 as `mantissa * 2^exponent`, the mantissa an integer below 2^53.
struct decoded
{
    bool negative = false;
    u64 mantissa = 0;
    int exponent = 0;
};

decoded decode(f64 x)
{
    auto const bits = cc::bit_cast<u64>(x);
    auto const field = int((bits >> 52) & 0x7ff);
    auto const fraction = bits & ((u64(1) << 52) - 1);
    if (field == 0)
        return {.negative = (bits >> 63) != 0, .mantissa = fraction, .exponent = -1074};
    return {.negative = (bits >> 63) != 0, .mantissa = fraction | (u64(1) << 52), .exponent = field - 1075};
}

int leading_zeroes(cc::u128 v)
{
    return v.hi != 0 ? cc::count_leading_zeroes(v.hi) : 64 + cc::count_leading_zeroes(v.lo);
}

cc::u128 shifted_left(cc::u128 v, int n)
{
    if (n == 0)
        return v;
    if (n >= 64)
        return {.lo = 0, .hi = v.lo << (n - 64)};
    return {.lo = v.lo << n, .hi = (v.hi << n) | (v.lo >> (64 - n))};
}

/// `v >> n`, with every bit shifted out folded into the lowest one kept, so a later rounding still sees it.
cc::u128 shifted_right_sticky(cc::u128 v, int n)
{
    if (n == 0)
        return v;
    if (n >= 128)
        return {.lo = (v.lo | v.hi) != 0 ? 1u : 0u, .hi = 0};
    auto lost = u64(0);
    auto kept = cc::u128();
    if (n >= 64)
    {
        lost = v.lo | (n > 64 ? v.hi << (128 - n) : 0);
        kept = {.lo = n > 64 ? v.hi >> (n - 64) : v.hi, .hi = 0};
    }
    else
    {
        lost = v.lo << (64 - n);
        kept = {.lo = (v.lo >> n) | (v.hi << (64 - n)), .hi = v.hi >> n};
    }
    if (lost != 0)
        kept.lo |= 1;
    return kept;
}

bool less(cc::u128 a, cc::u128 b)
{
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}
cc::u128 plus(cc::u128 a, cc::u128 b)
{
    auto const low = cc::add_with_carry(a.lo, b.lo);
    return {.lo = low.value, .hi = a.hi + b.hi + low.carry};
}
cc::u128 minus(cc::u128 a, cc::u128 b)
{
    auto const low = cc::sub_with_borrow(a.lo, b.lo);
    return {.lo = low.value, .hi = a.hi - b.hi - low.borrow};
}

bool bit_of(cc::u128 v, int i)
{
    return ((i >= 64 ? v.hi >> (i - 64) : v.lo >> i) & 1) != 0;
}
/// Whether any of the bits below `i` is set.
bool any_below(cc::u128 v, int i)
{
    if (i <= 0)
        return false;
    if (i >= 128)
        return (v.lo | v.hi) != 0;
    if (i >= 64)
        return v.lo != 0 || (i > 64 && (v.hi << (128 - i)) != 0);
    return (v.lo << (64 - i)) != 0;
}

/// `r * 2^e`, `r` nonzero, rounded once to the nearest f64, ties to even, with the sign given.
/// The bit an alignment folded what it shifted out into lies below the guard bit, so it reads as sticky.
f64 rounded(bool negative, cc::u128 r, int e)
{
    auto const top = 127 - leading_zeroes(r);
    // keep 53 bits, or fewer where the result is subnormal and its exponent is pinned at -1074
    auto shift = top - 52;
    if (e + shift < -1074)
        shift = -1074 - e;
    auto mantissa = u64(0);
    if (shift <= 0)
        mantissa = shifted_left(r, -shift).lo;
    else if (shift <= 128)
    {
        if (shift < 64)
            mantissa = (r.lo >> shift) | (r.hi << (64 - shift));
        else if (shift < 128)
            mantissa = r.hi >> (shift - 64);
        auto const guard = bit_of(r, shift - 1);
        if (guard && (any_below(r, shift - 1) || (mantissa & 1) != 0))
            ++mantissa;
    }
    auto exponent = e + shift;
    if (mantissa == (u64(1) << 53))
    {
        mantissa >>= 1;
        ++exponent;
    }
    auto bits = u64(negative ? 1 : 0) << 63;
    if (mantissa >= (u64(1) << 52))
    {
        auto const biased = exponent + 1075;
        if (biased >= 2047)
            return negative ? -infinity() : infinity();
        bits |= (u64(biased) << 52) | (mantissa & ((u64(1) << 52) - 1));
    }
    else
        bits |= mantissa;
    return cc::bit_cast<f64>(bits);
}
} // namespace

f64 impl::soft_fma(f64 a, f64 b, f64 c)
{
    // a product of a NaN or an infinity is exactly what it is, and a finite one plus an infinity is that infinity
    if (!is_finite(a) || !is_finite(b))
        return a * b + c;
    if (!is_finite(c))
        return c;
    // an exact zero product: the sum is IEEE's own, signs of zero and all
    if (a == 0.0 || b == 0.0)
        return a * b + c;

    auto const x = decode(a);
    auto const y = decode(b);
    auto const z = decode(c);
    auto const product_negative = x.negative != y.negative;
    // both terms as 128-bit integers whose top bit is bit 125, leaving room for the carry of a sum
    auto p = cc::umul128(x.mantissa, y.mantissa);
    auto pe = x.exponent + y.exponent;
    auto const p_shift = leading_zeroes(p) - 2;
    p = shifted_left(p, p_shift);
    pe -= p_shift;
    if (z.mantissa == 0)
        return rounded(product_negative, p, pe);
    auto q = cc::u128{.lo = z.mantissa, .hi = 0};
    auto qe = z.exponent;
    auto const q_shift = leading_zeroes(q) - 2;
    q = shifted_left(q, q_shift);
    qe -= q_shift;
    auto e = pe;
    if (pe >= qe)
        q = shifted_right_sticky(q, pe - qe);
    else
    {
        p = shifted_right_sticky(p, qe - pe);
        e = qe;
    }
    if (product_negative == z.negative)
        return rounded(product_negative, plus(p, q), e);
    if (p.hi == q.hi && p.lo == q.lo)
        return 0.0; // an exact cancellation is +0 when rounding to nearest
    return less(q, p) ? rounded(product_negative, minus(p, q), e) : rounded(z.negative, minus(q, p), e);
}

f64 impl::fma_to_odd(f64 a, f64 b, f64 c)
{
    auto const product = a * b;
    auto const sum = product + c;
    if (!is_finite(sum) || !is_finite(product) || !is_finite(c))
        return sum;
    // the error of the sum, exactly (Knuth's TwoSum), so `sum + error` is `a * b + c`
    auto const b_virtual = sum - product;
    auto const error = (product - (sum - b_virtual)) + (c - b_virtual);
    if (error == 0.0 || sum == 0.0)
        return sum;
    // round to odd: of the two f64 either side of the exact value, the one whose last bit is set
    auto bits = cc::bit_cast<u64>(sum);
    if ((bits & 1) == 0)
        bits = (error > 0.0) == (sum > 0.0) ? bits + 1 : bits - 1;
    return cc::bit_cast<f64>(bits);
}
