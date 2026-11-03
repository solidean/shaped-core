#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/bigrat/bigrat.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/symbolic/multi_polynomial.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::multi_polynomial;
using tg::vars;

namespace
{
bigint random_coefficient(cc::random& rng)
{
    switch (rng.next_u64() % 4)
    {
    case 0:
        return bigint(i64(rng.next_u64() % 7) - 3);
    case 1:
        return bigint(i64(rng.next_u64() % 2001) - 1000);
    default:
    {
        u64 limbs[2] = {rng.next_u64(), rng.next_u64()};
        return bigint::from_limbs(cc::span<u64 const>(limbs, 1 + isize(rng.next_u64() % 2)), rng.next_u64() % 2 == 0);
    }
    }
}

/// Up to max_terms terms with exponents up to max_exponent, repeats and cancellations included.
template <class P>
P random_poly(cc::random& rng, int max_terms, int max_exponent)
{
    P p;
    auto const n = int(rng.next_u64() % u64(max_terms + 1));
    for (auto t = 0; t < n; ++t)
    {
        typename P::exponents_t e;
        for (auto v = 0; v < P::variable_count; ++v)
            e[v] = int(rng.next_u64() % u64(max_exponent + 1));
        p += P::monomial(random_coefficient(rng), e);
    }
    return p;
}

template <class P>
cc::fixed_array<bigint, P::variable_count> random_point(cc::random& rng)
{
    cc::fixed_array<bigint, P::variable_count> x;
    for (auto v = 0; v < P::variable_count; ++v)
        x[v] = bigint(i64(rng.next_u64() % 11) - 5);
    return x;
}

/// Sorted, no zero coefficient, no repeated monomial: the representation's invariant.
template <class P>
bool is_canonical(P const& p)
{
    using V = typename P::vars_t;
    for (auto t = 0; t < p.term_count(); ++t)
    {
        if (tg::traits::is_zero(p.coefficient(t)))
            return false;
        if (t > 0 && !V::greater(V::pack(p.exponents(t - 1)), V::pack(p.exponents(t))))
            return false;
    }
    return true;
}

template <class P>
void check_ring_against_evaluation(cc::random& rng, int rounds)
{
    for (auto i = 0; i < rounds; ++i)
    {
        auto const a = random_poly<P>(rng, 8, 3);
        auto const b = random_poly<P>(rng, 8, 3);
        auto const c = random_poly<P>(rng, 4, 2);
        auto const x = random_point<P>(rng);

        CHECK((a + b)(x) == a(x) + b(x));
        CHECK((a - b)(x) == a(x) - b(x));
        CHECK((a * b)(x) == a(x) * b(x));
        CHECK((a * b * c)(x) == a(x) * b(x) * c(x));
        CHECK((-a)(x) == -a(x));
        CHECK((a * 7)(x) == a(x) * 7);

        CHECK(a * b == b * a);
        CHECK((a + b) * c == a * c + b * c);
        CHECK((a - a).is_zero());
        CHECK(is_canonical(a * b));
        CHECK(is_canonical(a - b));
    }
}

template <class P>
P make(i64 c, typename P::exponents_t const& e)
{
    return P::monomial(bigint(c), e);
}
} // namespace

TEST("tg multi_polynomial - packing: fields per word, words per monomial")
{
    static_assert(vars<3>::words == 1);                          // degree + 3 fields of 16 bits
    static_assert(vars<4>::words == 2);                          // degree + 4 fields
    static_assert(vars<4, tg::infinitesimal_order>::words == 1); // no degree field
    static_assert(vars<8, tg::lex_order, 8>::words == 1);
    static_assert(vars<5, tg::lex_order>::words == 2);
    static_assert(vars<3, tg::grevlex_order, 32>::words == 2);
    static_assert(vars<3>::max_exponent == 32767);

    using V = vars<5, tg::lex_order>;
    auto const e = V::exponents{3, 0, 7, 1, 2};
    CHECK(V::unpack(V::pack(e))[2] == 7);
    CHECK(V::unpack(V::multiply(V::pack(e), V::pack(e)))[4] == 4);
}

TEST("tg multi_polynomial - the orders rank monomials as their definitions say")
{
    using lex = vars<3, tg::lex_order>;
    using grlex = vars<3, tg::grlex_order>;
    using grevlex = vars<3, tg::grevlex_order>;
    using inf = vars<3, tg::infinitesimal_order>;

    // lex: x0 beats any power of x1
    CHECK(lex::greater(lex::pack({1, 0, 0}), lex::pack({0, 5, 0})));
    // grlex: the degree first, then lex
    CHECK(grlex::greater(grlex::pack({0, 2, 0}), grlex::pack({1, 0, 0})));
    CHECK(grlex::greater(grlex::pack({1, 0, 1}), grlex::pack({0, 2, 0})));
    // grevlex: at equal degree the smaller exponent of the last variable wins
    CHECK(grevlex::greater(grevlex::pack({0, 2, 0}), grevlex::pack({1, 0, 1})));
    CHECK(grevlex::greater(grevlex::pack({2, 0, 0}), grevlex::pack({0, 2, 0})));

    // infinitesimal: 1 >> e0 >> e0^2 >> e1 >> e0*e1 >> e1^2 >> e2
    auto const chain = cc::fixed_array<inf::exponents, 7>{{
        {0, 0, 0},
        {1, 0, 0},
        {2, 0, 0},
        {0, 1, 0},
        {1, 1, 0},
        {0, 2, 0},
        {0, 0, 1},
    }};
    for (auto i = 0; i + 1 < 7; ++i)
        CHECK(inf::greater(inf::pack(chain[i]), inf::pack(chain[i + 1])));
    CHECK(inf::greater(inf::pack({5, 0, 0}), inf::pack({0, 1, 0})));
}

TEST("tg multi_polynomial - ring operations agree with evaluation, under every order and word count")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 1500 : 200;
    check_ring_against_evaluation<multi_polynomial<bigint, vars<3>>>(rng, n);
    check_ring_against_evaluation<multi_polynomial<bigint, vars<3, tg::lex_order>>>(rng, n);
    check_ring_against_evaluation<multi_polynomial<bigint, vars<3, tg::grlex_order>>>(rng, n);
    check_ring_against_evaluation<multi_polynomial<bigint, vars<4, tg::infinitesimal_order>>>(rng, n);
    check_ring_against_evaluation<multi_polynomial<bigint, vars<5, tg::grevlex_order>>>(rng, n);
    check_ring_against_evaluation<multi_polynomial<bigint, vars<2, tg::lex_order, 8>>>(rng, n);
}

TEST("tg multi_polynomial - construction, observers and printing")
{
    using P = multi_polynomial<bigint, vars<3, tg::lex_order>>;
    auto const g = P::generators();
    auto const& x = g[0];
    auto const& y = g[1];
    auto const& z = g[2];

    auto const p = 3 * x * x * y - y + 1;
    CHECK(p.term_count() == 3);
    CHECK(p.to_string() == "3*x0^2*x1 - x1 + 1");
    CHECK(p.to_string(cc::fixed_array<cc::string_view, 3>{{"x", "y", "z"}}) == "3*x^2*y - y + 1");
    CHECK(p.leading_coefficient() == bigint(3));
    CHECK(p.leading_exponents()[0] == 2);
    CHECK(p.coefficient_of({0, 1, 0}) == bigint(-1));
    CHECK(p.coefficient_of({0, 0, 1}) == bigint(0));
    CHECK(p.degree(0) == 2);
    CHECK(p.degree(2) == 0);
    CHECK(p.total_degree() == 3);
    CHECK(P().total_degree() == -1);
    CHECK(P(0).is_zero());
    CHECK(P(5).is_constant());
    CHECK(p({2, 3, 4}) == bigint(3 * 4 * 3 - 3 + 1));
    CHECK((z * z - z * z).is_zero());
}

TEST("tg multi_polynomial - derivative, substitution and the univariate view")
{
    using P = multi_polynomial<bigint, vars<3>>;
    auto rng = nx::test_random();
    for (auto i = 0; i < 200; ++i)
    {
        auto const a = random_poly<P>(rng, 6, 3);
        auto const b = random_poly<P>(rng, 6, 3);
        auto const v = int(rng.next_u64() % 3);
        auto const x = random_point<P>(rng);

        // the product rule
        CHECK(derivative(a * b, v) == derivative(a, v) * b + a * derivative(b, v));
        CHECK(is_canonical(derivative(a, v)));

        // substituting is evaluating in one variable
        auto const s = substitute(a, v, x[v]);
        CHECK(s.degree(v) <= 0);
        CHECK(s(x) == a(x));
        CHECK(is_canonical(s));

        // a polynomial in x(v) over the others, and back
        auto const u = to_univariate(a, v);
        CHECK(from_univariate(u, v) == a);
        CHECK(u.degree() == a.degree(v));
    }

    // d/dx0 (x0^3 x1 + 2 x0 x2^2) = 3 x0^2 x1 + 2 x2^2
    auto const p = make<P>(1, {3, 1, 0}) + make<P>(2, {1, 0, 2});
    CHECK(derivative(p, 0) == make<P>(3, {2, 1, 0}) + make<P>(2, {0, 0, 2}));
}

TEST("tg multi_polynomial - the infinitesimal sign agrees with exact evaluation at hierarchically tiny values")
{
    // x(i) = t^w(i), w(i) = (D + 1)^i: a monomial's power of t reads its exponents as base-(D + 1) digits, x(N-1) the most significant,
    // so the dominant term has the smallest power, and t = 1 / T with T > 2 terms max|c| makes it outweigh the rest together.
    using P = multi_polynomial<bigint, vars<3, tg::infinitesimal_order>>;
    auto constexpr max_exponent = 2;
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 1000 : 150;
    for (auto i = 0; i < n; ++i)
    {
        auto const p = random_poly<P>(rng, 6, max_exponent) * random_poly<P>(rng, 3, 1);
        auto const degree_bound = 2 * max_exponent + 1;

        bigint largest = 1;
        for (auto t = 0; t < p.term_count(); ++t)
            if (auto const m = abs(p.coefficient(t)); m > largest)
                largest = m;
        auto const big = bigint(2 * (p.term_count() + 1)) * largest + 1;
        auto const t = bigrat(bigint(1), big);

        cc::fixed_array<bigrat, 3> x;
        auto w = 1;
        for (auto v = 0; v < 3; ++v)
        {
            x[v] = bigrat(1);
            for (auto k = 0; k < w; ++k)
                x[v] = x[v] * t;
            w *= degree_bound + 1;
        }
        CHECK(p.sign() == p(x).sign());
    }

    auto const e = P::generators();
    CHECK(P(0).sign() == 0);
    CHECK((e[1] - e[0]).sign() == -1);
    CHECK((e[1] * 1000 - e[0] * e[0]).sign() == -1);
    CHECK((e[0] * e[1] - e[1]).sign() == -1);
    CHECK((P(1) - e[0] * 1000000).sign() == 1);
}

TEST("tg multi_polynomial - nesting, and coefficients other than bigint")
{
    // a polynomial in infinitesimals over polynomials in the inputs
    using inputs = multi_polynomial<bigint, vars<2, tg::lex_order>>;
    using eps = multi_polynomial<inputs, vars<2, tg::infinitesimal_order>>;
    auto const a = inputs::generators();
    auto const e = eps::generators();

    auto const p = (eps(a[0]) + e[0]) * (eps(a[1]) - e[1]);
    CHECK(p.term_count() == 4);
    CHECK(p.leading_coefficient() == a[0] * a[1]);
    CHECK(p.to_string(cc::fixed_array<cc::string_view, 2>{{"e0", "e1"}}) == "x0*x1 + x1*e0 - x0*e1 - e0*e1");

    // fixed-width coefficients, wrapping as their arithmetic does
    using Q = multi_polynomial<tg::fi256, vars<3, tg::infinitesimal_order>>;
    auto const q = Q::generators();
    auto const r = (Q(tg::fi256(5)) + q[2]) * (Q(tg::fi256(-2)) + q[0]);
    CHECK(r.sign() == -1);
    CHECK(r.term_count() == 4);
    static_assert(tg::traits::is_exact<Q>);
    static_assert(!tg::traits::is_field<Q>);
}
