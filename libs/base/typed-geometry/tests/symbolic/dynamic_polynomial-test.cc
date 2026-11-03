#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/symbolic/dynamic_polynomial.hh>
#include <typed-geometry/symbolic/multi_polynomial.hh>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::symbol;

namespace
{
using poly = tg::multi_polynomial<bigint, tg::dynamic_vars<>>;

poly parse(cc::string_view text)
{
    auto p = poly::parse(text);
    REQUIRE(p.has_value());
    return p.value();
}

bigint small(cc::random& rng)
{
    return bigint(i64(rng.next_u64() % 41) - 20);
}
} // namespace

TEST("tg dynamic polynomial - parsing and printing")
{
    CHECK(parse("3*x^2*y - y + 1").to_string() == "3*x^2*y - y + 1");
    CHECK(parse("(x + 1)^2").to_string() == "x^2 + 2*x + 1");
    CHECK(parse("-(a - b) * 2").to_string() == "-2*a + 2*b");
    CHECK(parse("0").is_zero());
    CHECK(parse("x - x").is_zero());
    CHECK(parse("  12345678901234567890123 * z ").to_string() == "12345678901234567890123*z");
    CHECK(!poly::parse("x +").has_value());
    CHECK(!poly::parse("(x").has_value());
    CHECK(!poly::parse("x ^ y").has_value());
    CHECK(!poly::parse("2 ** x").has_value());

    auto const p = parse("x^3*y + 2*x*z^2");
    auto const x = symbol("x");
    auto const y = symbol("y");
    auto const z = symbol("z");
    CHECK(p.degree(x) == 3);
    CHECK(p.degree(symbol("w")) == 0);
    CHECK(p.total_degree() == 4);
    CHECK(p.variables().size() == 3);
    CHECK(p({{x, bigint(2)}, {y, bigint(3)}, {z, bigint(5)}}) == bigint(8 * 3 + 2 * 2 * 25));
}

TEST("tg dynamic polynomial - operands over different variables are repacked onto their union")
{
    auto const x = poly::variable(symbol("x"));
    auto const y = poly::variable(symbol("y"));
    CHECK(((x + y) - y) == x);
    CHECK((x * y).to_string() == "x*y");
    CHECK(((x + 1) * (y - 1)).to_string() == "x*y - x + y - 1");
    CHECK(((x + y) * (x - y)) == parse("x^2 - y^2"));
    // equality does not care which variables a polynomial was packed over
    CHECK(parse("a + b - b") == parse("a"));
    CHECK(!(parse("a") == parse("b")));
}

TEST("tg dynamic polynomial - agrees with the static type, term for term")
{
    using fixed = tg::multi_polynomial<bigint, tg::vars<3>>;
    auto const names = cc::fixed_array<cc::string_view, 3>{{"a", "b", "c"}};
    auto const syms = cc::fixed_array<symbol, 3>{{symbol("a"), symbol("b"), symbol("c")}};
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 1000 : 150;
    for (auto i = 0; i < n; ++i)
    {
        // the same random polynomials, built in both
        auto const make = [&](fixed& f, poly& d)
        {
            for (auto t = 0; t < 6; ++t)
            {
                auto const c = small(rng);
                auto const e
                    = fixed::exponents_t{int(rng.next_u64() % 4), int(rng.next_u64() % 4), int(rng.next_u64() % 4)};
                f += fixed::monomial(c, e);
                auto m = poly(c);
                for (auto v = 0; v < 3; ++v)
                    for (auto k = 0; k < e[v]; ++k)
                        m *= poly::variable(syms[v]);
                d += m;
            }
        };
        fixed fa, fb;
        poly da, db;
        make(fa, da);
        make(fb, db);
        CHECK((fa * fb).to_string(names) == (da * db).to_string());
        CHECK((fa + fb).to_string(names) == (da + db).to_string());
        CHECK((fa - fb).to_string(names) == (da - db).to_string());
        CHECK(derivative(fa, 1).to_string(names) == derivative(da, syms[1]).to_string());
    }
}

TEST("tg dynamic polynomial - many variables, derivative and substitution")
{
    // nine variables need three words a term
    auto rng = nx::test_random();
    cc::vector<symbol> names;
    for (auto v = 0; v < 9; ++v)
        names.push_back(symbol(cc::format("v{}", v)));
    for (auto i = 0; i < 100; ++i)
    {
        auto const random = [&]
        {
            poly p;
            for (auto t = 0; t < 5; ++t)
            {
                auto m = poly(small(rng));
                for (auto k = 0; k < 3; ++k)
                    m *= poly::variable(names[rng.next_u64() % 9]);
                p += m;
            }
            return p;
        };
        auto const a = random();
        auto const b = random();
        cc::vector<tg::assignment<bigint>> at;
        for (auto const& s : names)
            at.push_back({.variable = s, .value = small(rng)});
        auto const span = cc::span<tg::assignment<bigint> const>(at);
        CHECK((a * b).evaluate(span) == a.evaluate(span) * b.evaluate(span));
        CHECK((a + b).evaluate(span) == a.evaluate(span) + b.evaluate(span));
        CHECK(derivative(a * b, names[2]) == derivative(a, names[2]) * b + a * derivative(b, names[2]));
    }

    auto const x = symbol("x");
    auto const y = symbol("y");
    CHECK(substitute(parse("x^2 + y"), x, parse("y + 1")) == parse("y^2 + 3*y + 1"));
    CHECK(substitute(parse("x*y"), x, poly(bigint(4))) == parse("4*y"));
    CHECK(derivative(parse("x^3*y + y^2"), x) == parse("3*x^2*y"));
    CHECK(derivative(parse("y"), x).is_zero());
}
