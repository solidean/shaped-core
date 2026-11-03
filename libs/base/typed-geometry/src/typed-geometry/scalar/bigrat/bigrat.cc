#include "bigrat.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/float.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::bigrat_unreduced;

namespace
{
/// Temporaries kept per thread, so a stream of operations reuses their nodes rather than allocating.
/// No operation calls another while it holds one, except _reduce, which takes only g.
struct scratch
{
    bigint g;
    bigint h;
    bigint a1;
    bigint b1;
    bigint t;
    bigint u;
    bigint v;
    bigint w;
    /// Never written: a denominator of 1 to point at.
    bigint empty;
};
thread_local scratch tls;

/// x = 0, keeping its node; a denominator of 1.
void set_zero(bigint& x)
{
    if (!x.is_zero())
        x.assign_sub(x, x);
}

/// r = x * y for denominators, zero standing for 1 in each.
void mul_den(bigint& r, bigint const& x, bigint const& y)
{
    if (x.is_zero())
        r = y;
    else if (y.is_zero())
        r = x;
    else
        r.assign_mul(x, y);
}

/// r = x ± y.
void add_or_sub(bigint& r, bigint const& x, bigint const& y, bool subtract)
{
    if (subtract)
        r.assign_sub(x, y);
    else
        r.assign_add(x, y);
}
} // namespace

bigrat::bigrat(bigint num, bigint const& den) : _num(cc::move(num)), _den(den)
{
    CC_ASSERT(!_den.is_zero(), "a rational's denominator is nonzero");
    _reduce();
}

void bigrat::_reduce()
{
    if (_den.is_negative())
    {
        _den.negate();
        _num.negate();
    }
    if (_num.is_zero())
    {
        set_zero(_den);
        return;
    }
    auto& g = tls.g;
    gcd(g, _num, _den);
    if (!g.is_one())
    {
        div_exact(_num, _num, g);
        div_exact(_den, _den, g);
    }
    if (_den.is_one())
        set_zero(_den);
}

bigrat bigrat::from_f64(f64 x)
{
    CC_ASSERT(cc::is_finite(x), "a rational holds finite values only");
    auto const bits = cc::bit_cast<u64>(x);
    auto const negative = (bits >> 63) != 0;
    auto const biased = int((bits >> 52) & 0x7ff);
    auto mantissa = bits & ((u64(1) << 52) - 1);
    // a normal value has the hidden bit; a subnormal one the exponent of the smallest normal
    if (biased != 0)
        mantissa |= u64(1) << 52;
    auto exponent = (biased == 0 ? 1 : biased) - 1075;
    bigrat r;
    if (mantissa == 0)
        return r;
    // mantissa * 2^exponent with the twos the mantissa holds moved into the exponent, so a negative one leaves an odd numerator
    auto const twos = cc::count_trailing_zeroes(mantissa);
    mantissa >>= twos;
    exponent += twos;
    r._num = bigint(mantissa);
    if (exponent > 0)
        r._num <<= i64(exponent);
    else if (exponent < 0)
        r._den = bigint(1) << i64(-exponent);
    if (negative)
        r._num.negate();
    return r;
}

f64 bigrat::to_f64() const
{
    if (_den.is_zero())
        return _num.to_f64();
    // the quotient to 66 bits or more, and a sticky bit below it for an inexact one, so its rounding to 53 bits is the quotient's
    auto& n = tls.t;
    auto& d = tls.u;
    n = abs(_num);
    d = _den;
    auto const shift = i64(66) + d.bit_width() - n.bit_width();
    if (shift > 0)
        n <<= shift;
    else if (shift < 0)
        d <<= -shift;
    auto& q = tls.v;
    auto& r = tls.w;
    div_mod(q, r, n, d);
    if (!r.is_zero())
    {
        q <<= 1;
        q.assign_add(q, bigint(1));
    }
    auto const extra = r.is_zero() ? i64(0) : i64(1);
    // q is below 2^70, so its own rounding is to 53 bits, and the scaling by a power of two is exact unless it leaves the normal range
    auto const value = tg::scale_by_pow2(q.to_f64(), -(shift + extra));
    return _num.is_negative() ? -value : value;
}

cc::string bigrat::to_string() const
{
    auto s = _num.to_string();
    if (!_den.is_zero())
    {
        s += "/";
        s += _den.to_string();
    }
    return s;
}

void bigrat::_assign_sum(bigrat const& a, bigrat const& b, bool subtract)
{
    auto const& an = a._num;
    auto const& ad = a._den;
    auto const& bn = b._num;
    auto const& bd = b._den;

    if (ad.is_zero() && bd.is_zero())
    {
        // integers: a bigint sum, and *this's denominator, zero already when *this is an operand, cleared
        add_or_sub(_num, an, bn, subtract);
        set_zero(_den);
        return;
    }
    if (bd.is_zero() || ad.is_zero())
    {
        // x ± n with x = p / q: (p ± n q) / q, reduced already, since gcd(p ± n q, q) = gcd(p, q) = 1
        auto const b_integer = bd.is_zero();
        auto const& p = b_integer ? an : bn;
        auto const& q = b_integer ? ad : bd;
        auto const& n = b_integer ? bn : an;
        auto& t = tls.t;
        t.assign_mul(n, q);
        auto& v = tls.v;
        // a - n: p - nq; n - b: nq - p
        if (b_integer)
            add_or_sub(v, p, t, subtract);
        else
            add_or_sub(v, t, p, subtract);
        auto& w = tls.w;
        w = q;
        cc::swap(_num, v);
        cc::swap(_den, w);
        return;
    }

    // a/b ± c/d: by g = gcd(b, d), then g2 = gcd(t, g) for t = a (d/g) ± c (b/g); the result is (t/g2) / ((b/g) (d/g2))
    auto& g = tls.g;
    gcd(g, ad, bd);
    auto& t = tls.t;
    auto& u = tls.u;
    auto& v = tls.v;
    auto& w = tls.w;
    if (g.is_one())
    {
        t.assign_mul(an, bd);
        u.assign_mul(bn, ad);
        add_or_sub(v, t, u, subtract);
        w.assign_mul(ad, bd);
        cc::swap(_num, v);
        cc::swap(_den, w);
        return;
    }
    auto& a1 = tls.a1;
    auto& b1 = tls.b1;
    div_exact(a1, ad, g);
    div_exact(b1, bd, g);
    t.assign_mul(an, b1);
    u.assign_mul(bn, a1);
    add_or_sub(v, t, u, subtract);
    if (v.is_zero())
    {
        set_zero(_num);
        set_zero(_den);
        return;
    }
    auto& g2 = tls.h;
    gcd(g2, v, g);
    if (g2.is_one())
        w.assign_mul(a1, bd);
    else
    {
        div_exact(v, v, g2);
        div_exact(u, bd, g2);
        w.assign_mul(a1, u);
    }
    cc::swap(_num, v);
    cc::swap(_den, w);
    if (_den.is_one())
        set_zero(_den);
}

void bigrat::assign_mul(bigrat const& a, bigrat const& b)
{
    if (a.is_zero() || b.is_zero())
    {
        set_zero(_num);
        set_zero(_den);
        return;
    }
    if (a._den.is_zero() && b._den.is_zero())
    {
        _num.assign_mul(a._num, b._num);
        set_zero(_den);
        return;
    }
    _assign_cross(a._num, a._den, b._num, b._den);
}

void bigrat::assign_div(bigrat const& a, bigrat const& b)
{
    CC_ASSERT(!b.is_zero(), "division by zero");
    if (a.is_zero())
    {
        set_zero(_num);
        set_zero(_den);
        return;
    }
    // a times b's reciprocal, b's denominator over b's numerator, whose sign _assign_cross moves to the numerator
    _assign_cross(a._num, a._den, b._den, b._num);
}

void bigrat::_assign_cross(bigint const& p, bigint const& q, bigint const& r, bigint const& s)
{
    // (p/q) (r/s) = ((p/g1) (r/g2)) / ((q/g2) (s/g1)) for g1 = gcd(p, s) and g2 = gcd(r, q),
    // two gcds of half the size of the one a product would need after
    // zero stands for 1 in q and s and, for a division, in r; a division's s carries the divisor's sign
    auto const& one = tls.empty;
    bigint const* n1 = &p;
    bigint const* d2 = &s;
    if (!s.is_zero())
    {
        auto& g1 = tls.g;
        gcd(g1, p, s);
        if (!g1.is_one())
        {
            div_exact(tls.a1, p, g1);
            div_exact(tls.t, s, g1);
            n1 = &tls.a1;
            d2 = tls.t.is_one() ? &one : &tls.t;
        }
    }
    bigint const* n2 = r.is_zero() ? nullptr : &r;
    bigint const* d1 = &q;
    if (!q.is_zero() && n2 != nullptr)
    {
        auto& g2 = tls.h;
        gcd(g2, r, q);
        if (!g2.is_one())
        {
            div_exact(tls.b1, r, g2);
            div_exact(tls.u, q, g2);
            n2 = &tls.b1;
            d1 = tls.u.is_one() ? &one : &tls.u;
        }
    }
    auto& v = tls.v;
    auto& w = tls.w;
    if (n2 == nullptr)
        v = *n1;
    else
        v.assign_mul(*n1, *n2);
    mul_den(w, *d1, *d2);
    cc::swap(_num, v);
    cc::swap(_den, w);
    if (_den.is_negative())
    {
        _den.negate();
        _num.negate();
    }
    if (_den.is_one())
        set_zero(_den);
}

int tg::compare(bigrat const& a, bigrat const& b)
{
    auto const sa = a.sign();
    auto const sb = b.sign();
    if (sa != sb)
        return sa < sb ? -1 : 1;
    if (sa == 0)
        return 0;
    if (a._den.is_zero() && b._den.is_zero())
        return compare(a._num, b._num);
    // a/b against c/d by a d against c b; the products' bit widths differ by at most one from the sums of their parts'
    // a zero denominator stands for 1, one bit wide
    auto const lhs_bits = a._num.bit_width() + (b._den.is_zero() ? 1 : b._den.bit_width());
    auto const rhs_bits = b._num.bit_width() + (a._den.is_zero() ? 1 : a._den.bit_width());
    if (lhs_bits > rhs_bits + 1)
        return sa;
    if (rhs_bits > lhs_bits + 1)
        return -sa;
    auto& l = tls.v;
    auto& r = tls.w;
    if (b._den.is_zero())
        l = a._num;
    else
        l.assign_mul(a._num, b._den);
    if (a._den.is_zero())
        r = b._num;
    else
        r.assign_mul(b._num, a._den);
    return compare(l, r);
}

// unreduced

bigrat bigrat_unreduced::reduced() const&
{
    bigrat r;
    r._num = num;
    r._den = den;
    if (!r._den.is_zero())
        r._reduce();
    return r;
}

bigrat bigrat_unreduced::reduced() &&
{
    bigrat r;
    r._num = cc::move(num);
    r._den = cc::move(den);
    if (!r._den.is_zero())
        r._reduce();
    return r;
}

void bigrat_unreduced::_assign_sum(bigrat_unreduced const& a, bigrat_unreduced const& b, bool subtract)
{
    if (a.den.is_zero() && b.den.is_zero())
    {
        add_or_sub(num, a.num, b.num, subtract);
        set_zero(den);
        return;
    }
    // a/b ± c/d = (a d ± c b) / (b d), zero denominators standing for 1
    auto& t = tls.t;
    auto& u = tls.u;
    if (b.den.is_zero())
        t = a.num;
    else
        t.assign_mul(a.num, b.den);
    if (a.den.is_zero())
        u = b.num;
    else
        u.assign_mul(b.num, a.den);
    auto& v = tls.v;
    auto& w = tls.w;
    add_or_sub(v, t, u, subtract);
    mul_den(w, a.den, b.den);
    cc::swap(num, v);
    cc::swap(den, w);
}

void bigrat_unreduced::assign_mul(bigrat_unreduced const& a, bigrat_unreduced const& b)
{
    auto& w = tls.w;
    mul_den(w, a.den, b.den);
    num.assign_mul(a.num, b.num);
    cc::swap(den, w);
}
