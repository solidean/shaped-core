#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/wide_arith.hh>
#include <typed-geometry/scalar/fwd.hh>

#include <compare>
#include <type_traits>

// The fixed_integer class template and the generic, hand-written bodies of every operation on it.
// Users include scalar/fixed_int/fixed_int.hh, never this header: the generated loop-free specializations of the
// *_op structs below must be visible before any of them is instantiated, and only that header guarantees it.
// libs/base/typed-geometry/docs/modules/scalar.md has the design.

#if defined(TG_CHECK_WIDE_ARITH_REQUESTED)
#define TG_CHECK_WIDE_ARITH 1
#else
#define TG_CHECK_WIDE_ARITH 0
#endif

namespace tg::impl
{
template <class T>
inline constexpr bool is_fixed_integer = false;
template <int Bits, bool Signed>
inline constexpr bool is_fixed_integer<fixed_integer<Bits, Signed>> = true;

/// A builtin integer that converts into a fixed_integer: not bool, not the character types.
template <class I>
concept builtin_integer
    = std::is_integral_v<I> && !std::is_same_v<I, bool> && !std::is_same_v<I, char> && !std::is_same_v<I, char8_t>
   && !std::is_same_v<I, char16_t> && !std::is_same_v<I, char32_t> && !std::is_same_v<I, wchar_t>;

/// Whether every value of I is a value of fixed_integer<Bits, Signed>, which is what makes the conversion implicit.
template <class I, int Bits, bool Signed>
inline constexpr bool builtin_fits
    = std::is_signed_v<I> ? Signed && int(sizeof(I) * 8) <= Bits : int(sizeof(I) * 8) < Bits + (Signed ? 0 : 1);

/// Number of 64-bit words a width is computed in; 32 bits is computed in one.
template <int Bits>
inline constexpr int word_count = Bits == 32 ? 1 : Bits / 64;

[[nodiscard]] constexpr u64 mask_if(bool b)
{
    return u64(0) - u64(b);
}

/// acc + a * b + carry as a 128-bit value; it cannot overflow, since (2^64 - 1)^2 + 2 * (2^64 - 1) = 2^128 - 1.
[[nodiscard]] constexpr cc::u128 mul_add(u64 acc, u64 a, u64 b, u64 carry)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    // One 128-bit expression lets the compiler schedule the whole carry chain, rather than three calls' worth.
    auto const s = static_cast<__uint128_t>(a) * b + acc + carry;
    return {u64(s), u64(s >> 64)};
#else
    auto const p = cc::umul128(a, b);
    auto const s1 = cc::add_with_carry(p.lo, acc);
    auto const s2 = cc::add_with_carry(s1.value, carry);
    return {s2.value, p.hi + s1.carry + s2.carry};
#endif
}

/// x's i-th 64-bit word, sign- or zero-extended past its top; for 32 bits that is the value widened to 64.
template <int Bits, bool Signed>
[[nodiscard]] constexpr u64 limb(fixed_integer<Bits, Signed> const& x, int i)
{
    if constexpr (Bits == 32)
    {
        u64 const w = Signed ? u64(i64(i32(x.limbs[0]))) : u64(x.limbs[0]);
        return i == 0 ? w : (Signed ? u64(i64(w) >> 63) : 0);
    }
    else
    {
        constexpr int n = Bits / 64;
        if (i < n)
            return x.limbs[i];
        return Signed ? u64(i64(x.limbs[n - 1]) >> 63) : 0;
    }
}

/// Stores x's i-th 64-bit word; for 32 bits only word 0 exists, and it keeps the low half.
template <int Bits, bool Signed>
constexpr void set_limb(fixed_integer<Bits, Signed>& x, int i, u64 v)
{
    if constexpr (Bits == 32)
        x.limbs[0] = u32(v);
    else
        x.limbs[i] = v;
}

template <int R, bool S, int A, int B>
[[nodiscard]] constexpr fixed_integer<R, S> add_generic(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
{
    fixed_integer<R, S> r;
    u64 carry = 0;
    for (auto i = 0; i < word_count<R>; ++i)
    {
        auto const s = cc::add_with_carry(limb(a, i), limb(b, i), carry);
        set_limb(r, i, s.value);
        carry = s.carry;
    }
    return r;
}

template <int R, bool S, int A, int B>
[[nodiscard]] constexpr fixed_integer<R, S> sub_generic(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
{
    fixed_integer<R, S> r;
    u64 borrow = 0;
    for (auto i = 0; i < word_count<R>; ++i)
    {
        auto const s = cc::sub_with_borrow(limb(a, i), limb(b, i), borrow);
        set_limb(r, i, s.value);
        borrow = s.borrow;
    }
    return r;
}

/// The product modulo 2^R, as the schoolbook product of the sign-extended words.
/// Deliberately a different formulation from the generated bodies (which multiply the raw words and correct for the
/// sign), so the two check each other.
template <int R, bool S, int A, int B>
[[nodiscard]] constexpr fixed_integer<R, S> mul_generic(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
{
    constexpr int n = word_count<R>;
    u64 w[n] = {};
    for (auto i = 0; i < n; ++i)
    {
        u64 carry = 0;
        auto const ai = limb(a, i);
        for (auto j = 0; i + j < n; ++j)
        {
            auto const p = mul_add(w[i + j], ai, limb(b, j), carry);
            w[i + j] = p.lo;
            carry = p.hi;
        }
    }
    fixed_integer<R, S> r;
    for (auto i = 0; i < n; ++i)
        set_limb(r, i, w[i]);
    return r;
}

template <int Bits, bool Signed>
[[nodiscard]] constexpr fixed_integer<Bits, Signed> shl_generic(fixed_integer<Bits, Signed> const& x, int n)
{
    fixed_integer<Bits, Signed> r;
    if constexpr (Bits == 32)
        r.limbs[0] = u32(x.limbs[0] << n);
    else
    {
        constexpr int count = Bits / 64;
        auto const word = n >> 6;
        auto const bit = n & 63;
        for (auto i = 0; i < count; ++i)
        {
            auto const hi = i - word >= 0 ? x.limbs[i - word] : u64(0);
            auto const lo = i - word - 1 >= 0 ? x.limbs[i - word - 1] : u64(0);
            // (lo >> 1) >> (63 - bit) is lo >> (64 - bit), and 0 rather than UB when bit is 0
            r.limbs[i] = (hi << bit) | ((lo >> 1) >> (63 - bit));
        }
    }
    return r;
}

/// Arithmetic for a signed value, logical for an unsigned one.
template <int Bits, bool Signed>
[[nodiscard]] constexpr fixed_integer<Bits, Signed> shr_generic(fixed_integer<Bits, Signed> const& x, int n)
{
    fixed_integer<Bits, Signed> r;
    if constexpr (Bits == 32)
        r.limbs[0] = Signed ? u32(i32(x.limbs[0]) >> n) : u32(x.limbs[0] >> n);
    else
    {
        constexpr int count = Bits / 64;
        auto const word = n >> 6;
        auto const bit = n & 63;
        for (auto i = 0; i < count; ++i)
        {
            auto const lo = limb(x, i + word); // limb() fills past the top with the sign
            auto const hi = limb(x, i + word + 1);
            r.limbs[i] = (lo >> bit) | ((hi << 1) << (63 - bit));
        }
    }
    return r;
}

/// a < b, as the borrow out of a - b; a signed comparison flips both sign bits first.
template <int Bits, bool Signed>
[[nodiscard]] constexpr bool less_generic(fixed_integer<Bits, Signed> const& a, fixed_integer<Bits, Signed> const& b)
{
    if constexpr (Bits == 32)
        return Signed ? i32(a.limbs[0]) < i32(b.limbs[0]) : a.limbs[0] < b.limbs[0];
    else
    {
        constexpr int count = Bits / 64;
        constexpr u64 flip = Signed ? u64(1) << 63 : 0;
        u64 borrow = 0;
        for (auto i = 0; i < count; ++i)
        {
            auto const ai = i == count - 1 ? a.limbs[i] ^ flip : a.limbs[i];
            auto const bi = i == count - 1 ? b.limbs[i] ^ flip : b.limbs[i];
            borrow = cc::sub_with_borrow(ai, bi, borrow).borrow;
        }
        return borrow != 0;
    }
}

template <int Bits, bool Signed>
[[nodiscard]] constexpr bool is_negative(fixed_integer<Bits, Signed> const& x)
{
    if constexpr (!Signed)
        return false;
    else
        return (limb(x, word_count<Bits> - 1) >> 63) != 0;
}

/// |x| as the unsigned type of the same width; the most negative value's magnitude fits there.
template <int Bits, bool Signed>
[[nodiscard]] constexpr fixed_integer<Bits, false> magnitude(fixed_integer<Bits, Signed> const& x)
{
    auto const u = fixed_integer<Bits, false>(x);
    if (!is_negative(x))
        return u;
    return sub_generic<Bits, false>(fixed_integer<Bits, false>(), u);
}

// --- division --------------------------------------------------------------------------------------------------

/// q = u / v and r = u % v over n-word unsigned values; v must be non-zero.
/// Knuth's Algorithm D with 64-bit digits, each digit estimated by cc::udiv128 and corrected at most twice.
template <int N>
constexpr void divmod_words(u64 const (&u)[N], u64 const (&v)[N], u64 (&q)[N], u64 (&r)[N])
{
    for (auto i = 0; i < N; ++i)
    {
        q[i] = 0;
        r[i] = 0;
    }

    auto n = N;
    while (n > 1 && v[n - 1] == 0)
        --n;
    auto m = N;
    while (m > 1 && u[m - 1] == 0)
        --m;

    if (n == 1)
    {
        u64 rem = 0;
        for (auto i = m - 1; i >= 0; --i)
        {
            auto const d = cc::udiv128({u[i], rem}, v[0]);
            q[i] = d.quotient;
            rem = d.remainder;
        }
        r[0] = rem;
        return;
    }
    if (m < n)
    {
        for (auto i = 0; i < N; ++i)
            r[i] = u[i];
        return;
    }

    auto const s = cc::count_leading_zeroes(v[n - 1]);
    auto const shift_in = [s](u64 hi, u64 lo) { return s == 0 ? hi : (hi << s) | (lo >> (64 - s)); };

    u64 vn[N] = {};
    for (auto i = n - 1; i > 0; --i)
        vn[i] = shift_in(v[i], v[i - 1]);
    vn[0] = v[0] << s;

    u64 un[N + 1] = {};
    un[m] = s == 0 ? 0 : u[m - 1] >> (64 - s);
    for (auto i = m - 1; i > 0; --i)
        un[i] = shift_in(u[i], u[i - 1]);
    un[0] = u[0] << s;

    for (auto j = m - n; j >= 0; --j)
    {
        // Estimate the digit from the top two words of the running remainder over the top divisor word.
        // The invariant un[j + n] <= vn[n - 1] holds, and equality is the one case udiv128 cannot take.
        u64 qhat = 0;
        u64 rhat = 0;
        bool rhat_overflow = false;
        if (un[j + n] >= vn[n - 1])
        {
            qhat = ~u64(0);
            auto const s1 = cc::add_with_carry(un[j + n - 1], vn[n - 1]);
            rhat = s1.value;
            rhat_overflow = s1.carry != 0;
        }
        else
        {
            auto const d = cc::udiv128({un[j + n - 1], un[j + n]}, vn[n - 1]);
            qhat = d.quotient;
            rhat = d.remainder;
        }
        while (!rhat_overflow)
        {
            auto const p = cc::umul128(qhat, vn[n - 2]);
            if (p.hi < rhat || (p.hi == rhat && p.lo <= un[j + n - 2]))
                break;
            --qhat;
            auto const s1 = cc::add_with_carry(rhat, vn[n - 1]);
            rhat = s1.value;
            rhat_overflow = s1.carry != 0;
        }

        // Multiply and subtract; a final borrow means the estimate was one too large, so add the divisor back.
        u64 borrow = 0;
        u64 carry = 0;
        for (auto i = 0; i < n; ++i)
        {
            auto const p = mul_add(0, qhat, vn[i], carry);
            carry = p.hi;
            auto const d = cc::sub_with_borrow(un[i + j], p.lo, borrow);
            un[i + j] = d.value;
            borrow = d.borrow;
        }
        auto const top = cc::sub_with_borrow(un[j + n], carry, borrow);
        un[j + n] = top.value;
        if (top.borrow != 0)
        {
            --qhat;
            u64 c = 0;
            for (auto i = 0; i < n; ++i)
            {
                auto const a = cc::add_with_carry(un[i + j], vn[i], c);
                un[i + j] = a.value;
                c = a.carry;
            }
            un[j + n] += c;
        }
        q[j] = qhat;
    }

    for (auto i = 0; i < n - 1; ++i)
        r[i] = s == 0 ? un[i] : (un[i] >> s) | (un[i + 1] << (64 - s));
    r[n - 1] = s == 0 ? un[n - 1] : (un[n - 1] >> s) | (un[n] << (64 - s));
}

template <int Bits, bool Signed>
struct div_mod_pair
{
    fixed_integer<Bits, Signed> quotient;
    fixed_integer<Bits, Signed> remainder;
};

/// Truncating division: the quotient rounds toward zero and the remainder takes the dividend's sign.
/// b must be non-zero; min() / -1 wraps to min().
template <int Bits, bool Signed>
[[nodiscard]] constexpr div_mod_pair<Bits, Signed> div_mod_trunc_generic(fixed_integer<Bits, Signed> const& a,
                                                                         fixed_integer<Bits, Signed> const& b)
{
    if constexpr (Bits == 32)
    {
        // i64 holds min() / -1, so the wrap happens in the truncation back to 32 bits
        if constexpr (Signed)
        {
            auto const x = i64(i32(a.limbs[0]));
            auto const y = i64(i32(b.limbs[0]));
            return {fixed_integer<32, true>(i32(u32(x / y))), fixed_integer<32, true>(i32(x % y))};
        }
        else
            return {fixed_integer<32, false>(a.limbs[0] / b.limbs[0]), fixed_integer<32, false>(a.limbs[0] % b.limbs[0])};
    }
    else
    {
        constexpr int count = Bits / 64;
        auto const ma = magnitude(a);
        auto const mb = magnitude(b);
        u64 q[count] = {};
        u64 r[count] = {};
        divmod_words<count>(ma.limbs, mb.limbs, q, r);

        fixed_integer<Bits, Signed> qs;
        fixed_integer<Bits, Signed> rs;
        for (auto i = 0; i < count; ++i)
        {
            qs.limbs[i] = q[i];
            rs.limbs[i] = r[i];
        }
        if (is_negative(a) != is_negative(b))
            qs = sub_generic<Bits, Signed>(fixed_integer<Bits, Signed>(), qs);
        if (is_negative(a))
            rs = sub_generic<Bits, Signed>(fixed_integer<Bits, Signed>(), rs);
        return {qs, rs};
    }
}

// --- floating point --------------------------------------------------------------------------------------------

/// 2^k as an f64, exactly; +infinity past the largest finite power.
[[nodiscard]] constexpr f64 pow2_f64(int k)
{
    if (k > 1023)
        return cc::bit_cast<f64>(u64(0x7ff) << 52);
    return cc::bit_cast<f64>(u64(1023 + k) << 52);
}

/// The top 64 significant bits of x's magnitude and the power of two they are scaled by.
/// With Sticky, bit 0 is ORed with every bit below them, which is what makes one rounding of the u64 correct.
template <bool Sticky, int Bits>
[[nodiscard]] constexpr cc::u128 top_bits(fixed_integer<Bits, false> const& m)
{
    constexpr int count = word_count<Bits>;
    auto t = count - 1;
    while (t > 0 && limb(m, t) == 0)
        --t;
    auto const top = limb(m, t);
    if (t == 0 || top == 0)
        return {top, 0};

    auto const msb = t * 64 + 63 - cc::count_leading_zeroes(top);
    auto const shift = msb - 63;
    auto const word = shift >> 6;
    auto const bit = shift & 63;
    auto chunk = (limb(m, word) >> bit) | ((limb(m, word + 1) << 1) << (63 - bit));
    if constexpr (Sticky)
    {
        auto below = bit == 0 ? u64(0) : limb(m, word) << (64 - bit);
        for (auto i = 0; i < word; ++i)
            below |= limb(m, i);
        chunk |= u64(below != 0);
    }
    return {chunk, u64(shift)};
}

/// Correctly rounded (to nearest, ties to even) unless Sticky is false, which may be one unit off near a tie.
template <bool Sticky, int Bits, bool Signed>
[[nodiscard]] constexpr f64 to_f64_generic(fixed_integer<Bits, Signed> const& x)
{
    auto const t = top_bits<Sticky>(magnitude(x));
    auto const v = f64(t.lo) * pow2_f64(int(t.hi));
    return is_negative(x) ? -v : v;
}

template <int Bits, bool Signed>
[[nodiscard]] constexpr f32 to_f32_generic(fixed_integer<Bits, Signed> const& x)
{
    auto const t = top_bits<true>(magnitude(x));
    // f32 overflows past 2^128, and the scale then is infinity whatever the chunk rounds to
    auto const v
        = t.hi > 127 ? cc::bit_cast<f32>(u32(0xff) << 23) : f32(t.lo) * cc::bit_cast<f32>(u32(127 + int(t.hi)) << 23);
    return is_negative(x) ? -v : v;
}

/// Whether the truncated value of v is a value of fixed_integer<Bits, Signed>; v must be finite.
template <int Bits, bool Signed>
[[nodiscard]] constexpr bool f64_fits(f64 v)
{
    auto const bits = cc::bit_cast<u64>(v);
    auto const negative = (bits >> 63) != 0;
    auto const e = int((bits >> 52) & 0x7ff) - 1023;
    auto const mantissa_zero = (bits & ((u64(1) << 52) - 1)) == 0;
    if (e < 0)
        return true; // |v| < 1 truncates to 0
    if constexpr (Signed)
        return e < Bits - 1 || (negative && e == Bits - 1 && mantissa_zero);
    else
        return !negative && e < Bits;
}

template <int Bits, bool Signed>
[[nodiscard]] constexpr fixed_integer<Bits, Signed> from_f64_generic(f64 v)
{
    auto const bits = cc::bit_cast<u64>(v);
    auto const negative = (bits >> 63) != 0;
    auto const e = int((bits >> 52) & 0x7ff) - 1023;
    if (e < 0 || e == 1024)
        return {}; // |v| < 1, or not finite
    auto const m = (bits & ((u64(1) << 52) - 1)) | (u64(1) << 52);

    fixed_integer<Bits, false> mag;
    if (e <= 52)
        set_limb(mag, 0, m >> (52 - e));
    else if (e < Bits)
    {
        set_limb(mag, 0, m);
        mag = shl_generic(mag, e - 52);
    }
    auto const r = fixed_integer<Bits, Signed>(mag);
    return negative ? sub_generic<Bits, Signed>(fixed_integer<Bits, Signed>(), r) : r;
}

// --- the specialization points ---------------------------------------------------------------------------------
// The primaries run the generic bodies; scalar/fixed_int/generated/ specializes them loop-free up to 256 bits.
// `generated` is what the tests read to pin that every table entry resolves to its specialization.

template <int R, int A, int B, bool S>
struct add_op
{
    static constexpr bool generated = false;
    [[nodiscard]] static constexpr fixed_integer<R, S> apply(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
    {
        return add_generic<R, S>(a, b);
    }
};

template <int R, int A, int B, bool S>
struct sub_op
{
    static constexpr bool generated = false;
    [[nodiscard]] static constexpr fixed_integer<R, S> apply(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
    {
        return sub_generic<R, S>(a, b);
    }
};

template <int R, int A, int B, bool S>
struct mul_op
{
    static constexpr bool generated = false;
    [[nodiscard]] static constexpr fixed_integer<R, S> apply(fixed_integer<A, S> const& a, fixed_integer<B, S> const& b)
    {
        return mul_generic<R, S>(a, b);
    }
};

template <int Bits, bool Signed>
struct shl_op
{
    static constexpr bool generated = false;
    [[nodiscard]] static constexpr fixed_integer<Bits, Signed> apply(fixed_integer<Bits, Signed> const& x, int n)
    {
        return shl_generic(x, n);
    }
};

template <int Bits, bool Signed>
struct shr_op
{
    static constexpr bool generated = false;
    [[nodiscard]] static constexpr fixed_integer<Bits, Signed> apply(fixed_integer<Bits, Signed> const& x, int n)
    {
        return shr_generic(x, n);
    }
};

inline constexpr void check_shift(int n, int bits)
{
#if TG_CHECK_WIDE_ARITH
    CC_ASSERT_ALWAYS(n >= 0 && n < bits, "fixed_int shift amount must be in [0, Bits)");
#else
    (void)n;
    (void)bits;
#endif
}
} // namespace tg::impl

/// A two's-complement integer of exactly Bits bits, signed or not; every operator wraps modulo 2^Bits.
///
/// Spelled tg::fixed_int<Bits> / tg::fixed_uint<Bits>, or tg::fi128, tg::fu192, ….
/// Bits is 32 or a multiple of 64.
/// Operators take the same type on both sides, and every change of width is written out:
/// the constructor widens, truncated_to<T>() narrows, and tg::add / sub / mul<R> in fixed_arith.hh compute
/// across widths.
/// limbs is least significant first, and the sign is the top bit of the last limb; fi32's one limb is a u32.
template <int Bits, bool Signed>
struct tg::impl::fixed_integer
{
    static_assert(Bits == 32 || (Bits >= 64 && Bits % 64 == 0), "fixed_int takes 32 or a multiple of 64 bits");

    using limb_type = std::conditional_t<Bits == 32, u32, u64>;
    static constexpr int bits = Bits;
    static constexpr bool is_signed = Signed;
    static constexpr int limb_count = word_count<Bits>;

    limb_type limbs[limb_count] = {};

    constexpr fixed_integer() = default;

    /// Implicit when every value of I fits, explicit (and wrapping) otherwise.
    template <builtin_integer I>
    constexpr explicit(!builtin_fits<I, Bits, Signed>) fixed_integer(I v)
    {
        auto const w = std::is_signed_v<I> ? u64(i64(v)) : u64(v);
        auto const fill = std::is_signed_v<I> && v < 0 ? ~u64(0) : u64(0);
        for (auto i = 0; i < limb_count; ++i)
            set_limb(*this, i, i == 0 ? w : fill);
    }

    constexpr fixed_integer(cc::i128 v)
        requires(Bits == 128 && Signed)
      : limbs{v.lo, u64(v.hi)}
    {
    }

    constexpr fixed_integer(cc::u128 v)
        requires(Bits == 128 && !Signed)
      : limbs{v.lo, v.hi}
    {
    }

    /// Widening, which is lossless: from a narrower value of the same signedness, or a narrower unsigned one.
    template <int B, bool S>
        requires(B < Bits && (S == Signed || !S))
    constexpr explicit fixed_integer(fixed_integer<B, S> const& x)
    {
        for (auto i = 0; i < limb_count; ++i)
            set_limb(*this, i, limb(x, i));
    }

    /// The same bits, read with the other signedness.
    template <bool S>
        requires(S != Signed)
    constexpr explicit fixed_integer(fixed_integer<Bits, S> const& x)
    {
        for (auto i = 0; i < limb_count; ++i)
            limbs[i] = x.limbs[i];
    }

    /// Truncates toward zero.
    /// v must be finite and its truncation must fit, which TG_CHECK_WIDE_ARITH checks.
    constexpr explicit fixed_integer(f64 v)
    {
#if TG_CHECK_WIDE_ARITH
        CC_ASSERT_ALWAYS((v - v == 0.0 && f64_fits<Bits, Signed>(v)), "fixed_int from f64: value is not finite or does "
                                                                      "not fit");
#endif
        *this = from_f64_generic<Bits, Signed>(v);
    }

    constexpr explicit fixed_integer(f32 v) : fixed_integer(f64(v)) {}

    [[nodiscard]] static constexpr fixed_integer min()
    {
        fixed_integer r;
        if constexpr (Signed)
            set_limb(r, limb_count - 1, Bits == 32 ? u64(1) << 31 : u64(1) << 63);
        return r;
    }

    [[nodiscard]] static constexpr fixed_integer max() { return ~min(); }

    // --- width changes ---

    template <class T>
        requires(is_fixed_integer<T> && T::bits > Bits)
    [[nodiscard]] constexpr T widened() const
    {
        return T(*this);
    }

    /// The low T::bits bits, read as T; T must be no wider and of the same signedness.
    template <class T>
        requires(is_fixed_integer<T> && T::bits <= Bits && T::is_signed == Signed)
    [[nodiscard]] constexpr T truncated_to() const
    {
        T r;
        for (auto i = 0; i < T::limb_count; ++i)
            set_limb(r, i, limb(*this, i));
        return r;
    }

    /// *this widened to T, then shifted left by n; n must be in [0, T::bits).
    template <class T>
        requires(is_fixed_integer<T> && T::bits >= Bits && T::is_signed == Signed)
    [[nodiscard]] constexpr T shifted_left(int n) const
    {
        check_shift(n, T::bits);
        if constexpr (T::bits == Bits)
            return shl_op<Bits, Signed>::apply(*this, n);
        else
            return shl_op<T::bits, Signed>::apply(T(*this), n);
    }

    // --- queries ---

    [[nodiscard]] constexpr bool is_negative() const { return impl::is_negative(*this); }

    /// -1, 0 or +1, without a branch: the sign of a determinant is what an exact predicate returns.
    [[nodiscard]] constexpr int sign() const
    {
        limb_type any = 0;
        for (auto i = 0; i < limb_count; ++i)
            any |= limbs[i];
        return int(any != 0) - 2 * int(is_negative());
    }

    /// Correctly rounded, to nearest with ties to even.
    [[nodiscard]] constexpr f64 to_f64() const { return to_f64_generic<true>(*this); }
    [[nodiscard]] constexpr f32 to_f32() const { return to_f32_generic(*this); }

    [[nodiscard]] constexpr int count_leading_zeroes() const
        requires(!Signed)
    {
        auto n = 0;
        for (auto i = limb_count - 1; i >= 0; --i)
        {
            if (limbs[i] != 0)
                return n + cc::count_leading_zeroes(limbs[i]);
            n += int(sizeof(limb_type) * 8);
        }
        return n;
    }

    [[nodiscard]] constexpr int count_leading_ones() const
        requires(!Signed)
    {
        return (~*this).count_leading_zeroes();
    }

    [[nodiscard]] constexpr int count_trailing_zeroes() const
        requires(!Signed)
    {
        auto n = 0;
        for (auto i = 0; i < limb_count; ++i)
        {
            if (limbs[i] != 0)
                return n + cc::count_trailing_zeroes(limbs[i]);
            n += int(sizeof(limb_type) * 8);
        }
        return n;
    }

    [[nodiscard]] constexpr int count_trailing_ones() const
        requires(!Signed)
    {
        return (~*this).count_trailing_zeroes();
    }

    [[nodiscard]] constexpr int popcount() const
        requires(!Signed)
    {
        auto n = 0;
        for (auto i = 0; i < limb_count; ++i)
            n += cc::popcount(limbs[i]);
        return n;
    }

    [[nodiscard]] constexpr int bit_width() const
        requires(!Signed)
    {
        return Bits - count_leading_zeroes();
    }

    [[nodiscard]] constexpr bool has_single_bit() const
        requires(!Signed)
    {
        return popcount() == 1;
    }

    /// The number of bits |*this| needs; min() needs all Bits of them.
    [[nodiscard]] constexpr int magnitude_bit_width() const
        requires(Signed)
    {
        return magnitude(*this).bit_width();
    }

    /// Decimal; the formatter in fixed_int.hh takes the full integer spec.
    [[nodiscard]] cc::string to_string() const;

    // --- operators, all modulo 2^Bits ---

    [[nodiscard]] friend constexpr fixed_integer operator+(fixed_integer const& a, fixed_integer const& b)
    {
        return add_op<Bits, Bits, Bits, Signed>::apply(a, b);
    }
    [[nodiscard]] friend constexpr fixed_integer operator-(fixed_integer const& a, fixed_integer const& b)
    {
        return sub_op<Bits, Bits, Bits, Signed>::apply(a, b);
    }
    [[nodiscard]] friend constexpr fixed_integer operator*(fixed_integer const& a, fixed_integer const& b)
    {
        return mul_op<Bits, Bits, Bits, Signed>::apply(a, b);
    }
    /// Truncates toward zero, like the builtins; b must be non-zero.
    [[nodiscard]] friend constexpr fixed_integer operator/(fixed_integer const& a, fixed_integer const& b)
    {
        CC_ASSERT(b != fixed_integer(), "fixed_int division by zero");
        return div_mod_trunc_generic(a, b).quotient;
    }
    /// Takes the dividend's sign, like the builtins; b must be non-zero.
    [[nodiscard]] friend constexpr fixed_integer operator%(fixed_integer const& a, fixed_integer const& b)
    {
        CC_ASSERT(b != fixed_integer(), "fixed_int division by zero");
        return div_mod_trunc_generic(a, b).remainder;
    }
    [[nodiscard]] friend constexpr fixed_integer operator-(fixed_integer const& x)
    {
        return sub_op<Bits, Bits, Bits, Signed>::apply(fixed_integer(), x);
    }
    [[nodiscard]] friend constexpr fixed_integer operator+(fixed_integer const& x) { return x; }

    [[nodiscard]] friend constexpr fixed_integer operator~(fixed_integer const& x)
    {
        fixed_integer r;
        for (auto i = 0; i < limb_count; ++i)
            r.limbs[i] = limb_type(~x.limbs[i]);
        return r;
    }
    [[nodiscard]] friend constexpr fixed_integer operator&(fixed_integer const& a, fixed_integer const& b)
    {
        fixed_integer r;
        for (auto i = 0; i < limb_count; ++i)
            r.limbs[i] = a.limbs[i] & b.limbs[i];
        return r;
    }
    [[nodiscard]] friend constexpr fixed_integer operator|(fixed_integer const& a, fixed_integer const& b)
    {
        fixed_integer r;
        for (auto i = 0; i < limb_count; ++i)
            r.limbs[i] = a.limbs[i] | b.limbs[i];
        return r;
    }
    [[nodiscard]] friend constexpr fixed_integer operator^(fixed_integer const& a, fixed_integer const& b)
    {
        fixed_integer r;
        for (auto i = 0; i < limb_count; ++i)
            r.limbs[i] = a.limbs[i] ^ b.limbs[i];
        return r;
    }

    /// n must be in [0, Bits), which TG_CHECK_WIDE_ARITH checks.
    [[nodiscard]] friend constexpr fixed_integer operator<<(fixed_integer const& x, int n)
    {
        check_shift(n, Bits);
        return shl_op<Bits, Signed>::apply(x, n);
    }
    /// Arithmetic on a signed value, logical on an unsigned one; n must be in [0, Bits).
    [[nodiscard]] friend constexpr fixed_integer operator>>(fixed_integer const& x, int n)
    {
        check_shift(n, Bits);
        return shr_op<Bits, Signed>::apply(x, n);
    }

    constexpr fixed_integer& operator+=(fixed_integer const& b) { return *this = *this + b; }
    constexpr fixed_integer& operator-=(fixed_integer const& b) { return *this = *this - b; }
    constexpr fixed_integer& operator*=(fixed_integer const& b) { return *this = *this * b; }
    constexpr fixed_integer& operator/=(fixed_integer const& b) { return *this = *this / b; }
    constexpr fixed_integer& operator%=(fixed_integer const& b) { return *this = *this % b; }
    constexpr fixed_integer& operator&=(fixed_integer const& b) { return *this = *this & b; }
    constexpr fixed_integer& operator|=(fixed_integer const& b) { return *this = *this | b; }
    constexpr fixed_integer& operator^=(fixed_integer const& b) { return *this = *this ^ b; }
    constexpr fixed_integer& operator<<=(int n) { return *this = *this << n; }
    constexpr fixed_integer& operator>>=(int n) { return *this = *this >> n; }

    [[nodiscard]] friend constexpr bool operator==(fixed_integer const& a, fixed_integer const& b) = default;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(fixed_integer const& a, fixed_integer const& b)
    {
        if (less_generic(a, b))
            return std::strong_ordering::less;
        if (less_generic(b, a))
            return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }
};
