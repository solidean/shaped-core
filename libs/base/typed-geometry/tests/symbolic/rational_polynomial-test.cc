#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigfloat/bigfloat.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/bigrat/bigrat.hh>
#include <typed-geometry/symbolic/multi_polynomial.hh>
#include <typed-geometry/symbolic/polynomial.hh>
#include <typed-geometry/symbolic/rational_polynomial.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::polynomial;

namespace
{
using poly_z = polynomial<bigint>;
using ratfun = tg::rational_polynomial<poly_z>;

poly_z random_poly(cc::random& rng, int max_degree)
{
    auto p = poly_z::zeroed(1 + int(rng.next_u64() % u64(max_degree + 1)));
    for (auto i = 0; i < p.stored_count(); ++i)
        p[i] = bigint(i64(rng.next_u64() % 11) - 5);
    p.trim();
    return p;
}

ratfun random_ratfun(cc::random& rng)
{
    auto den = random_poly(rng, 3);
    if (den.is_zero())
        den = poly_z(1);
    return ratfun(random_poly(rng, 3), den);
}
} // namespace

TEST("tg rational polynomial - reduced by the gcd, the denominator leading positively")
{
    auto const x = poly_z::x();
    auto const r = ratfun(x * x - 1, x - 1);
    CHECK(r.numerator() == x + 1);
    CHECK(r.denominator() == poly_z(1));
    CHECK(r.to_string() == "x + 1");

    auto const s = ratfun(x * 2, poly_z(-4));
    CHECK(s.numerator() == -x);
    CHECK(s.denominator() == poly_z(2));
    CHECK(s == ratfun(x * -3, poly_z(6)));
    CHECK(ratfun(poly_z(0), x + 5).denominator() == poly_z(1));

    // over an exact field the denominator is monic
    using ratfun_q = tg::rational_polynomial<polynomial<bigrat>>;
    auto const xq = polynomial<bigrat>::x();
    auto const q = ratfun_q(xq * bigrat(3), xq * bigrat(6) + bigrat(2));
    CHECK(q.denominator().leading_coefficient() == bigrat(1));

    static_assert(tg::traits::is_field<ratfun>);
    static_assert(ratfun::is_reduced);
}

TEST("tg rational polynomial - field operations agree with evaluation at rational points")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 1500 : 250;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = random_ratfun(rng);
        auto const b = random_ratfun(rng);
        auto const v = bigrat(bigint(i64(rng.next_u64() % 41) - 20), bigint(1 + i64(rng.next_u64() % 7)));
        // a point where neither denominator vanishes, nor b's numerator, which / divides by
        if (a.denominator()(v).is_zero() || b.denominator()(v).is_zero() || b.numerator()(v).is_zero())
            continue;
        CHECK((a + b)(v) == a(v) + b(v));
        CHECK((a - b)(v) == a(v) - b(v));
        CHECK((a * b)(v) == a(v) * b(v));
        if (!b.is_zero())
            CHECK((a / b)(v) == a(v) / b(v));
        CHECK(a - a == ratfun());
        // canonical: the same value has the same parts however it was reached
        CHECK((a + b) - b == a);
        if (!b.is_zero())
            CHECK((a * b) / b == a);
    }
}

TEST("tg rational polynomial - a polynomial over rational functions divides")
{
    // polynomials in y over Q(x): divrem holds, the quotient's coefficients rational functions of x
    using outer = polynomial<ratfun>;
    auto rng = nx::test_random();
    for (auto i = 0; i < 100; ++i)
    {
        auto a = outer::zeroed(4);
        auto b = outer::zeroed(2);
        for (auto k = 0; k < 4; ++k)
            a[k] = random_ratfun(rng);
        for (auto k = 0; k < 2; ++k)
            b[k] = random_ratfun(rng);
        a.trim();
        b.trim();
        if (b.is_zero())
            continue;
        auto const [q, r] = divrem(a, b);
        CHECK(q * b + r == a);
        CHECK(r.degree() < b.degree());
    }
}

TEST("tg rational polynomial - the sign over infinitesimals is the parts' signs")
{
    using eps = tg::multi_polynomial<bigint, tg::vars<2, tg::infinitesimal_order>>;
    using eps_ratio = tg::rational_polynomial<eps>;
    static_assert(eps_ratio::is_reduced); // by the multivariate gcd
    auto const e = eps::generators();
    // (e0 - 1) / (e1 - 2): negative over negative
    auto const r = eps_ratio(e[0] - 1, e[1] - 2);
    CHECK(r.sign() == 1);
    CHECK(r.denominator().leading_coefficient() == bigint(2)); // sign moved onto the numerator
    // (e0 - e1) / (3 + e1): the numerator's dominant term is e0
    CHECK(eps_ratio(e[0] - e[1], e[1] + 3).sign() == 1);
    CHECK(eps_ratio(e[1] - e[0], e[1] + 3).sign() == -1);
    // reduced by the multivariate gcd: a common factor goes, and equal values have equal parts
    CHECK(eps_ratio(e[0] * 2, eps(4)) == eps_ratio(e[0], eps(2)));
    auto const common = eps_ratio((e[0] - e[1]) * (e[0] + 3), (e[1] + 1) * (e[0] + 3));
    CHECK(common.numerator() == e[0] - e[1]);
    CHECK(common.denominator() == e[1] + 1);
    CHECK((eps_ratio(e[0], e[1] + 1) * eps_ratio(e[1] + 1, eps(1))) == eps_ratio(e[0]));
}

TEST("tg rational polynomial - over bigfloat and bigrat coefficients, a quotient of dyadic epsilon polynomials keeps its sign")
{
    // bigfloat has no division, so no gcd: the parts stay unreduced; over bigrat, a field, they reduce and go monic
    using eps_f = tg::multi_polynomial<tg::bigfloat, tg::vars<2, tg::infinitesimal_order>>;
    using eps_q = tg::multi_polynomial<bigrat, tg::vars<2, tg::infinitesimal_order>>;
    using ratio_f = tg::rational_polynomial<eps_f>;
    using ratio_q = tg::rational_polynomial<eps_q>;
    static_assert(!ratio_f::is_reduced);
    auto const ef = eps_f::generators();
    auto const eq = eps_q::generators();
    auto rng = cc::random(11);
    auto const dyadic = [&]
    {
        auto const m = f64(i64(rng.next_u64() % 2001) - 1000);
        return tg::scale_by_pow2(m, int(rng.next_u64() % 41) - 20);
    };
    for (auto i = 0; i < 500; ++i)
    {
        f64 c[8];
        for (auto& x : c)
            x = dyadic();
        // (c0 + c1 e0) / (c2 + c3 e1) - (c4 e0 e1 + c5) / (c6 + c7 e0), in both coefficient types
        auto const f = [&](auto const& e, auto k)
        {
            using P = std::decay_t<decltype(e[0])>;
            using R = tg::rational_polynomial<P>;
            auto const p = [&](f64 x) { return P(k(x)); };
            auto const d1 = p(c[2]) + p(c[3]) * e[1];
            auto const d2 = p(c[6]) + p(c[7]) * e[0];
            if (d1.is_zero() || d2.is_zero())
                return 2;
            auto const r = R(p(c[0]) + p(c[1]) * e[0], d1) - R(p(c[4]) * e[0] * e[1] + p(c[5]), d2);
            // a bool, since tg::bigfloat has no to_string for CHECK to print the parts with
            auto const cleared = r * R(d1) * R(d2) == R((p(c[0]) + p(c[1]) * e[0]) * d2 - (p(c[4]) * e[0] * e[1] + p(c[5])) * d1);
            CHECK(cleared);
            return r.sign();
        };
        auto const sf = f(ef, [](f64 x) { return tg::bigfloat(x); });
        auto const sq = f(eq, [](f64 x) { return bigrat::from_f64(x); });
        CHECK(sf == sq);
    }
}
