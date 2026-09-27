#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>

// Arithmetic across fixed_int widths, and division.
//
// tg::add / sub / mul<R>(a, b) compute the exact result and store it in R, whose width the caller picks from a bound
// they already know: tg::mul<fi192>(a, b) for two fi128 bounded by 2^80 and 2^90.
// R is a claim about the result, not a modulus: TG_CHECK_WIDE_ARITH (SC_CHECK_WIDE_ARITH in CMake) asserts it, and
// tg::checked_add / sub / mul<R> always check it.
// Without the flag an exact result that does not fit wraps modulo 2^R, and nothing is checked on the hot path.

/// A quotient with its remainder, from one long division.
template <class T>
struct tg::div_mod_result
{
    T quotient;
    T remainder;
};

/// floor(x / w) and ceil(x / w), from one estimate and one remainder.
template <class T>
struct tg::floor_ceil_result
{
    T floor;
    T ceil;
};

namespace tg
{
namespace impl
{
template <int A, int B>
inline constexpr int exact_sum_bits = (A > B ? A : B) == 32 ? 64 : (A > B ? A : B) + 64;

template <int A, int B>
inline constexpr int exact_product_bits = (A + B + 63) / 64 * 64;

/// Whether exact is a value of R.
/// A bool rather than an optional, so the checks built on it stay usable in a constant expression.
template <class R, int W, bool S>
[[nodiscard]] constexpr bool fits_in(fixed_integer<W, S> const& exact)
{
    if constexpr (R::bits >= W)
        return true;
    else
        return fixed_integer<W, S>(exact.template truncated_to<R>()) == exact;
}

/// exact as R, or none when it does not fit.
template <class R, int W, bool S>
[[nodiscard]] constexpr cc::optional<R> fits_or_none(fixed_integer<W, S> const& exact)
{
    if (!fits_in<R>(exact))
        return cc::nullopt;
    if constexpr (R::bits >= W)
        return R(exact);
    else
        return exact.template truncated_to<R>();
}

template <class R, bool S>
concept result_of = is_fixed_integer<R> && R::is_signed == S;
} // namespace impl

// --- checked: the exact result, or none when it does not fit R ---------------------------------------------------

template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr cc::optional<R> checked_add(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
    return impl::fits_or_none<R>(impl::add_generic<impl::exact_sum_bits<A, B>, S>(a, b));
}

template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr cc::optional<R> checked_sub(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
    return impl::fits_or_none<R>(impl::sub_generic<impl::exact_sum_bits<A, B>, S>(a, b));
}

template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr cc::optional<R> checked_mul(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
    return impl::fits_or_none<R>(impl::mul_generic<impl::exact_product_bits<A, B>, S>(a, b));
}

// --- heterogeneous: the exact result stored in R -----------------------------------------------------------------

/// a + b, which must fit R; checked under TG_CHECK_WIDE_ARITH, modulo 2^R otherwise.
template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr R add(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
#if TG_CHECK_WIDE_ARITH
    CC_ASSERT_ALWAYS(impl::fits_in<R>(impl::add_generic<impl::exact_sum_bits<A, B>, S>(a, b)),
                     "tg::add: the exact sum does not fit the result type");
#endif
    return impl::add_op<R::bits, A, B, S>::apply(a, b);
}

/// a - b, which must fit R; checked under TG_CHECK_WIDE_ARITH, modulo 2^R otherwise.
template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr R sub(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
#if TG_CHECK_WIDE_ARITH
    CC_ASSERT_ALWAYS(impl::fits_in<R>(impl::sub_generic<impl::exact_sum_bits<A, B>, S>(a, b)),
                     "tg::sub: the exact difference does not fit the result type");
#endif
    return impl::sub_op<R::bits, A, B, S>::apply(a, b);
}

/// a * b, which must fit R; checked under TG_CHECK_WIDE_ARITH, modulo 2^R otherwise.
template <class R, int A, int B, bool S>
    requires impl::result_of<R, S>
[[nodiscard]] constexpr R mul(impl::fixed_integer<A, S> const& a, impl::fixed_integer<B, S> const& b)
{
#if TG_CHECK_WIDE_ARITH
    CC_ASSERT_ALWAYS(impl::fits_in<R>(impl::mul_generic<impl::exact_product_bits<A, B>, S>(a, b)),
                     "tg::mul: the exact product does not fit the result type");
#endif
    return impl::mul_op<R::bits, A, B, S>::apply(a, b);
}

// --- division at one width ---------------------------------------------------------------------------------------
// b must be non-zero in all of them, and min() / -1 wraps to min().
// trunc rounds the quotient toward zero (the operators' rule), floor toward negative infinity, ceil toward positive.

template <int Bits, bool S>
[[nodiscard]] constexpr div_mod_result<impl::fixed_integer<Bits, S>> div_mod_trunc(impl::fixed_integer<Bits, S> const& a,
                                                                                   impl::fixed_integer<Bits, S> const& b)
{
    CC_ASSERT((b != impl::fixed_integer<Bits, S>()), "fixed_int division by zero");
    auto const r = impl::div_mod_trunc_generic(a, b);
    return {r.quotient, r.remainder};
}

template <int Bits, bool S>
[[nodiscard]] constexpr div_mod_result<impl::fixed_integer<Bits, S>> div_mod_floor(impl::fixed_integer<Bits, S> const& a,
                                                                                   impl::fixed_integer<Bits, S> const& b)
{
    auto r = div_mod_trunc(a, b);
    if (r.remainder != impl::fixed_integer<Bits, S>() && a.is_negative() != b.is_negative())
    {
        r.quotient -= impl::fixed_integer<Bits, S>(1);
        r.remainder += b;
    }
    return r;
}

template <int Bits, bool S>
[[nodiscard]] constexpr impl::fixed_integer<Bits, S> div_trunc(impl::fixed_integer<Bits, S> const& a,
                                                               impl::fixed_integer<Bits, S> const& b)
{
    return div_mod_trunc(a, b).quotient;
}

template <int Bits, bool S>
[[nodiscard]] constexpr impl::fixed_integer<Bits, S> mod_trunc(impl::fixed_integer<Bits, S> const& a,
                                                               impl::fixed_integer<Bits, S> const& b)
{
    return div_mod_trunc(a, b).remainder;
}

template <int Bits, bool S>
[[nodiscard]] constexpr impl::fixed_integer<Bits, S> div_floor(impl::fixed_integer<Bits, S> const& a,
                                                               impl::fixed_integer<Bits, S> const& b)
{
    return div_mod_floor(a, b).quotient;
}

/// Takes the divisor's sign.
template <int Bits, bool S>
[[nodiscard]] constexpr impl::fixed_integer<Bits, S> mod_floor(impl::fixed_integer<Bits, S> const& a,
                                                               impl::fixed_integer<Bits, S> const& b)
{
    return div_mod_floor(a, b).remainder;
}

template <int Bits, bool S>
[[nodiscard]] constexpr impl::fixed_integer<Bits, S> div_ceil(impl::fixed_integer<Bits, S> const& a,
                                                              impl::fixed_integer<Bits, S> const& b)
{
    auto const r = div_mod_trunc(a, b);
    if (r.remainder != impl::fixed_integer<Bits, S>() && a.is_negative() == b.is_negative())
        return r.quotient + impl::fixed_integer<Bits, S>(1);
    return r.quotient;
}

// --- a quotient known to be small --------------------------------------------------------------------------------

namespace impl
{
/// floor(x / w) and ceil(x / w) from an estimate t that is floor(x / w) or one off it.
/// The exact remainder says which: it is computed at a width where x - t * w cannot overflow, since |r| < 2|w|.
template <int A, int B, bool S>
[[nodiscard]] constexpr floor_ceil_result<i64> correct_quotient(fixed_integer<A, S> const& x,
                                                                fixed_integer<B, S> const& w,
                                                                i64 t)
{
    constexpr int W = A > B + 64 ? A : B + 64;
    using wide = fixed_integer<W, S>;

    if constexpr (!S)
        t = t < 0 ? 0 : t; // the true quotient is not negative, and a wrapped estimate would break the remainder

    auto r = sub_op<W, A, W, S>::apply(x, mul_op<W, 64, B, S>::apply(fixed_integer<64, S>(t), w));
    auto const wv = wide(w);
    auto const w_negative = w.is_negative();
    auto const r_below = w_negative ? wide() < r : r < wide();
    auto const r_above = w_negative ? !(wv < r) : !(r < wv);
    if (r_below)
    {
        --t;
        r += wv;
    }
    else if (r_above)
    {
        ++t;
        r -= wv;
    }
    return {t, t + i64(r != wide())};
}

/// floor(x / w) and ceil(x / w) as i64, for a quotient that fits 32 bits.
///
/// The top 64 bits of |w| divide |x| shifted right by the same amount, in one 128 ÷ 64 step.
/// Truncating both moves the ratio by far less than one, so the estimate is floor(|x / w|) or one off it, either way.
/// For a negative quotient, one less than its negation keeps it within one of the floor; correct_quotient settles which.
/// The 32-bit quotient is also what keeps the shifted |x| below 2^64 times the divisor word, as the step requires.
/// Measured against an f64 estimate with the same correction, this is ~20% faster on x64.
template <int A, int B, bool S>
[[nodiscard]] constexpr floor_ceil_result<i64> small_quotient(fixed_integer<A, S> const& x, fixed_integer<B, S> const& w)
{
    auto const mx = magnitude(x);
    auto const mw = magnitude(w);
    auto const width = mw.bit_width();
    auto const s = width > 64 ? width - 64 : 0;
    auto const wt = limb(mw >> s, 0);
    auto const xs = s < A ? mx >> s : fixed_integer<A, false>(); // an x narrower than the shift divides to 0
    auto const q = i64(cc::udiv128({limb(xs, 0), limb(xs, 1)}, wt).quotient);
    auto const t = is_negative(x) != is_negative(w) ? -q - 1 : q;
    return correct_quotient(x, w, t);
}

template <class Q, int A, int B, bool S>
[[nodiscard]] constexpr floor_ceil_result<Q> floor_ceil(fixed_integer<A, S> const& x, fixed_integer<B, S> const& w)
{
    CC_ASSERT((w != fixed_integer<B, S>()), "fixed_int division by zero");
    if constexpr (Q::bits == 32)
    {
        auto const r = small_quotient(x, w);
#if TG_CHECK_WIDE_ARITH
        constexpr auto lo = S ? i64(i32(0x80000000u)) : i64(0);
        constexpr auto hi = S ? i64(0x7fffffff) : i64(0xffffffffu);
        CC_ASSERT_ALWAYS(r.floor >= lo && r.ceil <= hi, "tg::div_floor / div_ceil: the quotient does not fit the "
                                                        "result type");
#endif
        return {Q(r.floor), Q(r.ceil)};
    }
    else
    {
        // No estimate resolves a wider quotient to within one, so divide exactly at the wider operand's width.
        constexpr int M = A > B ? A : B;
        auto const a = fixed_integer<M, S>(x);
        auto const b = fixed_integer<M, S>(w);
        auto const floor = div_floor(a, b);
        auto const ceil = div_ceil(a, b);
#if TG_CHECK_WIDE_ARITH
        CC_ASSERT_ALWAYS(fits_in<Q>(floor) && fits_in<Q>(ceil), "tg::div_floor / div_ceil: the quotient does not fit "
                                                                "the result type");
#endif
        return {floor.template truncated_to<Q>(), ceil.template truncated_to<Q>()};
    }
}
} // namespace impl

/// floor(x / w) for a quotient known to fit Q, which is the claim TG_CHECK_WIDE_ARITH checks; w must be non-zero.
/// With Q 32 bits wide this is one 128 ÷ 64 estimate plus one exact correction rather than a long division.
template <class Q, int A, int B, bool S>
    requires impl::result_of<Q, S>
[[nodiscard]] constexpr Q div_floor(impl::fixed_integer<A, S> const& x, impl::fixed_integer<B, S> const& w)
{
    return impl::floor_ceil<Q>(x, w).floor;
}

/// ceil(x / w) for a quotient known to fit Q; see div_floor<Q>.
template <class Q, int A, int B, bool S>
    requires impl::result_of<Q, S>
[[nodiscard]] constexpr Q div_ceil(impl::fixed_integer<A, S> const& x, impl::fixed_integer<B, S> const& w)
{
    return impl::floor_ceil<Q>(x, w).ceil;
}

/// Both, from one estimate and one remainder; see div_floor<Q>.
template <class Q, int A, int B, bool S>
    requires impl::result_of<Q, S>
[[nodiscard]] constexpr floor_ceil_result<Q> div_floor_ceil(impl::fixed_integer<A, S> const& x,
                                                            impl::fixed_integer<B, S> const& w)
{
    return impl::floor_ceil<Q>(x, w);
}
} // namespace tg
