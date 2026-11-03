#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/symbolic/polynomial_roots.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::polynomial;

namespace
{
using poly_z = polynomial<bigint>;
using poly_q = polynomial<bigrat>;

poly_q to_rational(poly_z const& p)
{
    auto q = poly_q::zeroed(p.degree() + 1);
    for (auto i = 0; i <= p.degree(); ++i)
        q[i] = bigrat(p[i]);
    return q;
}

/// The distinct real roots of p by Sturm's theorem, independent of Descartes: V(-inf) - V(+inf) along the Sturm chain.
int sturm_count(poly_z const& p)
{
    cc::vector<poly_q> chain;
    chain.push_back(to_rational(p));
    chain.push_back(derivative(chain[0]));
    while (!chain.back().is_zero())
    {
        auto r = divrem(chain[chain.size() - 2], chain.back()).remainder;
        chain.push_back(-poly_q(r));
    }
    chain.remove_back();
    auto const variations = [&](bool at_plus_infinity)
    {
        auto v = 0;
        auto last = 0;
        for (auto const& q : chain)
        {
            auto s = q.leading_coefficient().sign();
            if (!at_plus_infinity && q.degree() % 2 == 1)
                s = -s;
            if (last != 0 && s != last)
                ++v;
            last = s;
        }
        return v;
    };
    return variations(false) - variations(true);
}

bool holds(tg::root_interval const& r, bigrat const& x)
{
    return r.is_exact() ? r.lo == x : (r.lo < x && x < r.hi);
}
} // namespace

TEST("tg polynomial roots - isolation finds every known root once, in disjoint ascending intervals")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 600 : 100;
    for (auto i = 0; i < n; ++i)
    {
        // distinct rational roots b/a, some repeated, times a quadratic without real roots
        cc::vector<bigrat> known;
        auto p = poly_z(1);
        auto const count = 1 + int(rng.next_u64() % 5);
        for (auto k = 0; k < count; ++k)
        {
            auto const a = bigint(1 + i64(rng.next_u64() % 4));
            auto const b = bigint(i64(rng.next_u64() % 41) - 20);
            auto const r = bigrat(b, a);
            auto seen = false;
            for (auto const& x : known)
                seen = seen || x == r;
            auto const factor = poly_z{-b, a};
            p = p * factor;
            if (rng.next_u64() % 3 == 0)
                p = p * factor;
            if (!seen)
                known.push_back(r);
        }
        if (rng.next_u64() % 2 == 0)
            p = p * poly_z{1 + i64(rng.next_u64() % 5), 0, 1};

        auto const roots = isolate_real_roots(p);
        CHECK(roots.size() == known.size());
        for (auto const& x : known)
        {
            auto hits = 0;
            for (auto const& r : roots)
                hits += holds(r, x) ? 1 : 0;
            CHECK(hits == 1);
        }
        for (isize k = 1; k < roots.size(); ++k)
            CHECK(roots[k - 1].hi <= roots[k].lo);
    }
}

TEST("tg polynomial roots - isolation counts what Sturm's theorem counts, on random polynomials")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 600 : 120;
    for (auto i = 0; i < n; ++i)
    {
        auto p = poly_z::zeroed(2 + int(rng.next_u64() % 7));
        for (auto k = 0; k < p.stored_count(); ++k)
            p[k] = bigint(i64(rng.next_u64() % 2001) - 1000);
        p.trim();
        if (p.degree() <= 0)
            continue;
        auto const roots = isolate_real_roots(p);
        CHECK(int(roots.size()) == sturm_count(primitive_part(squarefree_part(p))));
        for (auto const& r : roots)
            if (!r.is_exact())
                CHECK(p(r.lo).sign() * p(r.hi).sign() <= 0);
    }
}

TEST("tg polynomial roots - refining an irrational root")
{
    // x^2 - 2: -sqrt(2) and sqrt(2)
    auto const p = poly_z{-2, 0, 1};
    auto const roots = isolate_real_roots(p);
    REQUIRE(roots.size() == 2);
    auto const r = refine_root(p, roots[1], bigrat(bigint(1), bigint(1) << 40));
    CHECK(r.hi - r.lo <= bigrat(bigint(1), bigint(1) << 40));
    CHECK(r.lo * r.lo < bigrat(2));
    CHECK(r.hi * r.hi > bigrat(2));
    CHECK(roots[0].hi <= bigrat(0));

    CHECK(isolate_real_roots(poly_z{1, 0, 1}).empty());
    CHECK(isolate_real_roots(poly_z(5)).empty());
    auto const zero = isolate_real_roots(poly_z{0, 0, 3});
    REQUIRE(zero.size() == 1);
    CHECK(zero[0].is_exact());
    CHECK(zero[0].lo == bigrat(0));
}

TEST("tg polynomial roots - floating-point roots on an interval")
{
    // a cubic with three roots in [0, 1], as a curve's parameter would have
    auto const cubic = polynomial<f64, 3>{-0.07, 0.59, -1.4, 1.0}; // (t - 0.2)(t - 0.5)(t - 0.7)
    auto const roots = real_roots(cubic, 0.0, 1.0);
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(roots)>, cc::fixed_vector<f64, 3>>);
    REQUIRE(roots.size() == 3);
    CHECK(tg::abs(roots[0] - 0.2) < 1e-12);
    CHECK(tg::abs(roots[1] - 0.5) < 1e-12);
    CHECK(tg::abs(roots[2] - 0.7) < 1e-12);
    CHECK(real_roots(cubic, 0.3, 0.6).size() == 1);

    // a double root where the polynomial is exactly zero is reported once
    auto const square = polynomial<f64, 2>{0.25, -1.0, 1.0};
    auto const double_root = real_roots(square, 0.0, 1.0);
    REQUIRE(double_root.size() == 1);
    CHECK(double_root[0] == 0.5);

    // random roots in [0, 1], kept apart
    auto rng = nx::test_random();
    for (auto i = 0; i < 300; ++i)
    {
        auto const count = 1 + int(rng.next_u64() % 6);
        cc::vector<f64> known;
        auto p = polynomial<f64>(1.0);
        while (int(known.size()) < count)
        {
            auto const r = f64(rng.next_u64() % 1000) / 1000.0;
            auto far = true;
            for (auto x : known)
                far = far && tg::abs(x - r) > 0.05;
            if (!far)
                continue;
            known.push_back(r);
            p = p * polynomial<f64>{-r, 1.0};
        }
        cc::sort(known);
        auto const found = real_roots(p, -0.5, 1.5);
        REQUIRE(int(found.size()) == count);
        for (auto k = 0; k < count; ++k)
            CHECK(tg::abs(found[k] - known[k]) < 1e-8);
    }
}
