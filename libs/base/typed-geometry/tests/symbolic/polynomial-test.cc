#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/bigrat/bigrat.hh>
#include <typed-geometry/symbolic/polynomial.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::polynomial;

namespace
{
using poly_z = polynomial<bigint>;
using poly_q = polynomial<bigrat>;

/// A coefficient of up to three limbs, zero or small as often as large, so cancellations and carries both happen.
bigint random_coefficient(cc::random& rng)
{
    switch (rng.next_u64() % 5)
    {
    case 0:
        return 0;
    case 1:
        return bigint(i64(rng.next_u64() % 7) - 3);
    default:
    {
        u64 limbs[3];
        auto const n = 1 + int(rng.next_u64() % 3);
        for (auto i = 0; i < n; ++i)
            limbs[i] = rng.next_u64();
        return bigint::from_limbs(cc::span<u64 const>(limbs, n), rng.next_u64() % 2 == 0);
    }
    }
}

poly_z random_poly(cc::random& rng, int max_degree)
{
    auto p = poly_z::zeroed(1 + int(rng.next_u64() % u64(max_degree + 1)));
    for (auto i = 0; i < p.stored_count(); ++i)
        p[i] = random_coefficient(rng);
    p.trim();
    return p;
}

poly_q to_rational(poly_z const& p)
{
    auto q = poly_q::zeroed(p.stored_count());
    for (auto i = 0; i < p.stored_count(); ++i)
        q[i] = bigrat(p[i]);
    q.trim();
    return q;
}

template <class P>
concept has_divrem = requires(P a) { divrem(a, a); };

bigint power(bigint const& b, int e)
{
    bigint r = 1;
    for (auto i = 0; i < e; ++i)
        r = r * b;
    return r;
}
} // namespace

TEST("tg polynomial - construction, degree and printing")
{
    CHECK(poly_z().is_zero());
    CHECK(poly_z().degree() == -1);
    CHECK(poly_z(0).is_zero());
    CHECK(poly_z(5).degree() == 0);
    CHECK((poly_z{1, 0, 3, 0, 0}.degree() == 2));
    CHECK((poly_z{1, 0, 3, 0, 0}.stored_count() == 3));
    CHECK(poly_z::x().degree() == 1);
    CHECK((poly_z::monomial(4, 3) == poly_z{0, 0, 0, 4}));

    CHECK(poly_z().to_string() == "0");
    CHECK((poly_z{1, -1, 3}.to_string() == "3*x^2 - x + 1"));
    CHECK((poly_z{-2, 0, 0, -1}.to_string("t") == "-t^3 - 2"));
    CHECK((poly_z{0, 1}.to_string() == "x"));

    // a fixed bound stores every coefficient and its degree is the highest non-zero one
    auto const cubic = polynomial<f64, 3>{1.0, 2.0};
    CHECK(cubic.stored_count() == 4);
    CHECK(cubic.degree() == 1);
    CHECK((cubic == polynomial<f64>{1.0, 2.0}));
}

TEST("tg polynomial - ring operations agree with evaluation at many points")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 3000 : 400;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = random_poly(rng, 8);
        auto const b = random_poly(rng, 8);
        auto const c = random_poly(rng, 4);
        auto const v = random_coefficient(rng);

        CHECK((a + b)(v) == a(v) + b(v));
        CHECK((a - b)(v) == a(v) - b(v));
        CHECK((a * b)(v) == a(v) * b(v));
        CHECK((-a)(v) == -a(v));
        CHECK((a * v)(bigint(3)) == a(bigint(3)) * v);

        CHECK(a * b == b * a);
        CHECK((a + b) * c == a * c + b * c);
        CHECK((a - b) + b == a);
        CHECK((a - a).is_zero());
        CHECK((a * b).degree() == (a.is_zero() || b.is_zero() ? -1 : a.degree() + b.degree()));

        // the stored coefficients end in a non-zero one after every operation
        auto const p = a * b - b * a + c;
        CHECK(p.stored_count() == p.degree() + 1);
    }
}

TEST("tg polynomial - divrem over a field recovers the dividend")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 2000 : 300;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = to_rational(random_poly(rng, 9));
        auto b = to_rational(random_poly(rng, 5));
        if (b.is_zero())
            b = poly_q{bigrat(1), bigrat(2)};

        auto const [q, r] = divrem(a, b);
        CHECK(q * b + r == a);
        CHECK(r.degree() < b.degree());
    }

    // over the integers there is no divrem, since the quotient leaves them
    static_assert(has_divrem<poly_q>);
    static_assert(!has_divrem<poly_z>);
}

TEST("tg polynomial - pseudo_divrem and div_exact over the integers")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 2000 : 300;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = random_poly(rng, 9);
        auto b = random_poly(rng, 5);
        if (b.is_zero())
            b = poly_z{3, -2};

        auto const pd = pseudo_divrem(a, b);
        CHECK(pd.quotient * b + pd.remainder == a * power(b.leading_coefficient(), pd.power));
        CHECK(pd.remainder.degree() < b.degree());
        CHECK(pd.power == (a.degree() >= b.degree() ? a.degree() - b.degree() + 1 : 0));

        CHECK(div_exact(a * b, b) == a);
    }
}

TEST("tg polynomial - fixed degree bounds propagate through the operators")
{
    using cubic = polynomial<f64, 3>;
    using quadratic = polynomial<f64, 2>;

    auto const p = cubic{1.0, -3.0, 0.0, 2.0};
    auto const q = quadratic{-1.0, 0.0, 1.0};

    static_assert(std::is_same_v<decltype(p * q), polynomial<f64, 5>>);
    static_assert(std::is_same_v<decltype(p + q), cubic>);
    static_assert(std::is_same_v<decltype(derivative(p)), quadratic>);
    static_assert(std::is_same_v<decltype(compose(p, q)), polynomial<f64, 6>>);
    static_assert(std::is_same_v<decltype(divrem(p, q).remainder), polynomial<f64, 1>>);
    static_assert(std::is_same_v<decltype(p * polynomial<f64>{}), polynomial<f64>>);

    CHECK(p(2.0) == 11.0);
    CHECK((derivative(p) == quadratic{-3.0, 0.0, 6.0}));
    CHECK((p * q)(3.0) == p(3.0) * q(3.0));
    CHECK(compose(p, q)(3.0) == p(q(3.0)));

    // x^3 - 1 = (x - 1)(x^2 + x + 1), exact in doubles
    auto const [quot, rem] = divrem(cubic{-1.0, 0.0, 0.0, 1.0}, quadratic{1.0, 1.0, 1.0});
    CHECK((quot == cubic{-1.0, 1.0}));
    CHECK(rem.is_zero());

    // narrowing back to a bound is explicit and holds when the degree fits
    auto const widened = polynomial<f64>(p) * 1.0;
    CHECK(cubic(widened) == p);
}

TEST("tg polynomial - composition and evaluation at another scalar")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 200; ++i)
    {
        auto const p = random_poly(rng, 4);
        auto const q = random_poly(rng, 3);
        auto const v = bigint(i64(rng.next_u64() % 41) - 20);
        CHECK(compose(p, q)(v) == p(q(v)));
    }

    // an integer polynomial at a rational evaluates in the rationals
    auto const p = poly_z{1, 2, 4};
    auto const half = bigrat(bigint(1), bigint(2));
    static_assert(std::is_same_v<decltype(p(half)), bigrat>);
    CHECK(p(half) == bigrat(3));

    CHECK((pow(poly_z{1, 1}, 4) == poly_z{1, 4, 6, 4, 1}));
    CHECK((pow(poly_z{1, 1}, 0) == poly_z(1)));
}

TEST("tg polynomial - a polynomial is a coefficient, so nesting gives two variables")
{
    using inner = poly_z;            // in x
    using outer = polynomial<inner>; // in y, over polynomials in x

    auto const x = outer(inner::x());
    auto const y = outer::x();
    auto const s = x + y;
    auto const sq = s * s;

    // (x + y)^2 = x^2 + 2xy + y^2
    CHECK(sq.degree() == 2);
    CHECK((sq[0] == inner{0, 0, 1}));
    CHECK((sq[1] == inner{0, 2}));
    CHECK(sq[2] == inner(1));
    CHECK(sq.to_string("y") == "y^2 + 2*x*y + x^2");
    CHECK((s * s * outer(inner{1, 1})).to_string("y") == "(x + 1)*y^2 + (2*x^2 + 2*x)*y + x^3 + x^2");

    // y = 3 leaves a polynomial in x
    CHECK((sq(inner(3)) == inner{9, 6, 1}));
    CHECK(tg::traits::is_zero(sq - s * s));
    static_assert(!tg::traits::is_field<outer>);
    static_assert(tg::traits::is_exact<outer>);

    // over a ring of polynomials there is pseudo-division
    auto const pd = pseudo_divrem(sq, s);
    CHECK(pd.remainder.is_zero());
    CHECK(pd.quotient == s);
}
