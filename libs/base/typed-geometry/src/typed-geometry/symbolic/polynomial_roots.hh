#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/vector.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/bigrat/bigrat.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/symbolic/polynomial.hh>

#include <type_traits>

// Real roots of univariate polynomials: exact isolation over the integers, and floating-point roots on an interval.
// libs/base/typed-geometry/docs/modules/symbolic.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// An interval holding exactly one real root of a polynomial: open (lo, hi), or the root itself where lo == hi.
struct tg::root_interval
{
    bigrat lo;
    bigrat hi;

    [[nodiscard]] bool is_exact() const { return lo == hi; }
};

namespace tg::impl
{
/// Sign changes along the coefficients, zeros skipped: Descartes' bound on the positive roots, exact when it is 0 or 1.
[[nodiscard]] inline int sign_variations(polynomial<bigint> const& p)
{
    auto v = 0;
    auto last = 0;
    for (auto const& c : p.coefficients())
    {
        auto const s = c.sign();
        if (s == 0)
            continue;
        if (last != 0 && s != last)
            ++v;
        last = s;
    }
    return v;
}

/// p(x + 1), in place: Horner's scheme for every coefficient, n^2 / 2 additions and nothing else.
inline void taylor_shift_by_one(polynomial<bigint>& p)
{
    auto const n = p.degree();
    for (auto i = 0; i < n; ++i)
        for (auto j = n - 1; j >= i; --j)
            p[j] += p[j + 1];
}

/// The number of roots in (0, 1): Descartes' rule on (x + 1)^n p(1 / (x + 1)), exact when it is 0 or 1.
[[nodiscard]] inline int roots_in_unit_interval_bound(polynomial<bigint> const& p)
{
    auto r = p;
    auto const n = r.degree();
    for (auto i = 0; i < n - i; ++i)
        cc::swap(r[i], r[n - i]);
    taylor_shift_by_one(r);
    return sign_variations(r);
}

/// The positive roots of a squarefree p with p(0) != 0, as intervals scaled by 2^k: p's roots lie in (0, 2^k).
inline void isolate_positive_roots(polynomial<bigint> const& p, int k, cc::vector<tg::root_interval>& out)
{
    // q(x) = p(2^k x) has its roots in (0, 1); each work item is q mapped from [c / 2^d, (c + 1) / 2^d] onto [0, 1]
    struct item
    {
        polynomial<bigint> q;
        bigint c;
        int d;
    };
    auto q = p;
    for (auto i = 1; i <= q.degree(); ++i)
        q[i] = q[i] << (i64(k) * i);

    auto const to_rational = [&](bigint const& c, int d) { return bigrat(c << k, bigint(1) << d); };
    cc::vector<item> work;
    work.push_back({.q = cc::move(q), .c = bigint(0), .d = 0});
    while (!work.empty())
    {
        auto it = work.pop_back();
        auto const v = roots_in_unit_interval_bound(it.q);
        if (v == 0)
            continue;
        if (v == 1)
        {
            out.push_back({.lo = to_rational(it.c, it.d), .hi = to_rational(it.c + 1, it.d)});
            continue;
        }
        // halves: left(x) = 2^n q(x / 2), right(x) = left(x + 1)
        auto const n = it.q.degree();
        auto left = cc::move(it.q);
        for (auto i = 0; i < n; ++i)
            left[i] = left[i] << i64(n - i);
        auto right = left;
        taylor_shift_by_one(right);
        auto const c2 = it.c * 2;
        if (right[0].is_zero())
        {
            // the midpoint is a root, rational: report it exactly and divide it out of the right half
            out.push_back({.lo = to_rational(c2 + 1, it.d + 1), .hi = to_rational(c2 + 1, it.d + 1)});
            auto shifted = polynomial<bigint>::zeroed(n);
            for (auto i = 1; i <= n; ++i)
                shifted[i - 1] = right[i];
            right = cc::move(shifted);
        }
        work.push_back({.q = cc::move(right), .c = c2 + 1, .d = it.d + 1});
        work.push_back({.q = cc::move(left), .c = c2, .d = it.d + 1});
    }
}

/// k with every root of p in (-2^k, 2^k), from Cauchy's bound 1 + max |a_i / a_n|.
[[nodiscard]] inline int root_bound_exponent(polynomial<bigint> const& p)
{
    i64 widest = 0;
    for (auto const& c : p.coefficients())
        widest = cc::max(widest, c.bit_width());
    return int(cc::max(i64(1), widest - p.leading_coefficient().bit_width() + 2));
}
} // namespace tg::impl

namespace tg
{
/// The real roots of p, each in its own interval, ascending: open intervals (lo, hi) with one root each,
/// or a single point lo == hi for a rational root met on the way.
/// A repeated root is reported once.
/// Descartes' rule of signs with bisection (Vincent, Collins and Akritas) on p's squarefree part, all in exact integers;
/// the intervals' endpoints are dyadic, and refine_root narrows one further.
template <int D>
[[nodiscard]] cc::vector<root_interval> isolate_real_roots(polynomial<bigint, D> const& input)
{
    cc::vector<root_interval> roots;
    if (input.degree() <= 0)
        return roots;
    auto p = primitive_part(squarefree_part(input));

    // a root at zero, divided out
    auto zero_root = false;
    if (p[0].is_zero())
    {
        zero_root = true;
        auto shifted = polynomial<bigint>::zeroed(p.degree());
        for (auto i = 1; i <= p.degree(); ++i)
            shifted[i - 1] = p[i];
        p = cc::move(shifted);
    }

    cc::vector<root_interval> negative;
    if (p.degree() > 0)
    {
        auto const k = impl::root_bound_exponent(p);
        impl::isolate_positive_roots(p, k, roots);
        // the negative roots are the positive ones of p(-x), mirrored
        auto m = p;
        for (auto i = 1; i <= m.degree(); i += 2)
            m[i] = -m[i];
        impl::isolate_positive_roots(m, k, negative);
    }

    cc::vector<root_interval> all;
    for (auto i = negative.size() - 1; i >= 0; --i)
        all.push_back({.lo = -negative[i].hi, .hi = -negative[i].lo});
    if (zero_root)
        all.push_back({.lo = bigrat(0), .hi = bigrat(0)});
    for (auto& r : roots)
        all.push_back(cc::move(r));
    // a midpoint root is recorded before the roots left of it, so the order comes from a sort;
    // the intervals are disjoint, and a point root sorts before the open interval starting at it
    cc::sort(all, [](root_interval const& x, root_interval const& y)
             { return x.lo < y.lo || (x.lo == y.lo && x.hi < y.hi); });
    return all;
}

/// r narrowed by bisection until hi - lo <= width, still holding p's single root; p must be the polynomial r was isolated for.
template <int D>
[[nodiscard]] root_interval refine_root(polynomial<bigint, D> const& p, root_interval r, bigrat const& width)
{
    if (r.is_exact())
        return r;
    auto const sign_at = [&](bigrat const& x) { return p(x).sign(); };
    auto const lo_sign = sign_at(r.lo);
    while (r.hi - r.lo > width)
    {
        auto const mid = (r.lo + r.hi) / bigrat(2);
        auto const s = sign_at(mid);
        if (s == 0)
            return {.lo = mid, .hi = mid};
        if (s == lo_sign)
            r.lo = mid;
        else
            r.hi = mid;
    }
    return r;
}
} // namespace tg

// floating-point roots on an interval

namespace tg::impl
{
template <class T, int D>
using roots_on_interval_t = std::conditional_t<D == dynamic_degree, cc::vector<T>, cc::fixed_vector<T, (D > 0 ? D : 1)>>;

/// The root of p in [a, b], where p(a) and p(b) differ in sign: Newton's steps, each kept inside the bracket by a bisection
/// when it would leave it, until the bracket stops shrinking.
template <class T, int D, int E>
[[nodiscard]] T bracketed_root(polynomial<T, D> const& p, polynomial<T, E> const& dp, T a, T b)
{
    auto fa = p(a);
    auto x = (a + b) / T(2);
    for (auto iteration = 0; iteration < 200; ++iteration)
    {
        auto const fx = p(x);
        if (fx == T(0))
            return x;
        // keep the half that holds the sign change
        if ((fx < T(0)) == (fa < T(0)))
        {
            a = x;
            fa = fx;
        }
        else
            b = x;
        auto const d = dp(x);
        auto next = d != T(0) ? x - fx / d : (a + b) / T(2);
        if (!(next > a && next < b))
            next = (a + b) / T(2);
        if (next == x || next == a || next == b)
            return next;
        x = next;
    }
    return x;
}

template <class T, int D>
void real_roots_on(polynomial<T, D> const& p, T lo, T hi, roots_on_interval_t<T, D>& out)
{
    auto const d = p.degree();
    if (d <= 0)
        return;
    if (d == 1)
    {
        auto const x = -p[0] / p[1];
        if (x >= lo && x <= hi)
            out.push_back(x);
        return;
    }
    // the derivative's roots cut [lo, hi] into pieces on which p is monotone: at most one root each
    auto const dp = derivative(p);
    roots_on_interval_t<T, polynomial_derivative_degree(D)> critical;
    real_roots_on(dp, lo, hi, critical);
    auto a = lo;
    auto fa = p(a);
    if (fa == T(0))
        out.push_back(a);
    for (auto i = 0; i <= int(critical.size()); ++i)
    {
        auto const b = i < int(critical.size()) ? critical[i] : hi;
        if (!(b > a))
            continue;
        auto const fb = p(b);
        if (fb == T(0))
            out.push_back(b);
        else if (fa != T(0) && (fa < T(0)) != (fb < T(0)))
            out.push_back(bracketed_root(p, dp, a, b));
        a = b;
        fa = fb;
    }
}
} // namespace tg::impl

namespace tg
{
/// The real roots of p in [lo, hi], ascending, for floating-point coefficients: as many as the degree, inline for a fixed one.
/// The derivative's roots, found the same way, cut the interval into monotone pieces, and each piece with a sign change
/// holds one root, found by bracketed Newton steps.
/// A root of even multiplicity is found only where p is exactly 0 there.
template <class T, int D>
    requires(!traits::is_exact<T>)
[[nodiscard]] impl::roots_on_interval_t<T, D> real_roots(polynomial<T, D> const& p, T lo, T hi)
{
    impl::roots_on_interval_t<T, D> out;
    impl::real_roots_on(p, lo, hi, out);
    return out;
}
} // namespace tg
