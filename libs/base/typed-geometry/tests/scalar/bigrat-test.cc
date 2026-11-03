#include <clean-core/math/bit.hh>
#include <clean-core/math/float.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigrat/bigrat.hh>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::bigrat;
using tg::bigrat_unreduced;

namespace
{
/// A limb that is all ones, zero or one as often as random, so carries run the whole length.
u64 random_limb(cc::random& rng)
{
    switch (rng.next_u64() % 6)
    {
    case 0:
        return 0;
    case 1:
        return ~u64(0);
    case 2:
        return 1;
    default:
        return rng.next_u64();
    }
}

/// A magnitude of 1 to max_limbs limbs, nonzero.
bigint random_magnitude(cc::random& rng, int max_limbs)
{
    u64 limbs[8];
    auto const n = 1 + int(rng.next_u64() % u64(max_limbs));
    for (auto i = 0; i < n; ++i)
        limbs[i] = random_limb(rng);
    limbs[n - 1] |= 1;
    // a small one now and then, where the common factors and the integers are
    if (rng.next_u64() % 4 == 0)
        return bigint(1 + rng.next_u64() % 12);
    return bigint::from_limbs(cc::span<u64 const>(limbs, isize(n)));
}

/// A numerator and a denominator, unreduced, with a common factor planted now and then; the denominator positive.
struct fraction
{
    bigint num;
    bigint den;
};

fraction random_fraction(cc::random& rng, int max_limbs)
{
    auto num = rng.next_u64() % 10 == 0 ? bigint(0) : random_magnitude(rng, max_limbs);
    if (rng.next_u64() % 2 == 0)
        num.negate();
    auto den = rng.next_u64() % 4 == 0 ? bigint(1) : random_magnitude(rng, max_limbs);
    if (rng.next_u64() % 3 == 0)
    {
        auto const f = random_magnitude(rng, 2);
        num = num * f;
        den = den * f;
    }
    return {num, den};
}

/// Whether r holds num / den, by the cross products, and is reduced with a positive denominator.
bool holds(bigrat const& r, bigint const& num, bigint const& den)
{
    auto const d = r.denominator();
    if (d.sign() <= 0)
        return false;
    if (!gcd(r.numerator(), d).is_one() && !(r.is_zero() && d.is_one()))
        return false;
    return r.numerator() * den == num * d;
}
} // namespace

TEST("tg bigrat - arithmetic agrees with the cross products and stays reduced")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 20000 : 3000;
    for (auto i = 0; i < n; ++i)
    {
        auto const max_limbs = 1 + int(rng.next_u64() % 5);
        auto const fa = random_fraction(rng, max_limbs);
        auto const fb = random_fraction(rng, max_limbs);
        auto const a = bigrat(fa.num, fa.den);
        auto const b = bigrat(fb.num, fb.den);
        CHECK(holds(a, fa.num, fa.den));
        CHECK(holds(b, fb.num, fb.den));
        CHECK(a.is_integer() == a.denominator().is_one());

        CHECK(holds(a + b, fa.num * fb.den + fb.num * fa.den, fa.den * fb.den));
        CHECK(holds(a - b, fa.num * fb.den - fb.num * fa.den, fa.den * fb.den));
        CHECK(holds(a * b, fa.num * fb.num, fa.den * fb.den));
        if (!b.is_zero())
        {
            // a / b with b's sign moved to the numerator
            auto num = fa.num * fb.den;
            auto den = fa.den * fb.num;
            if (den.is_negative())
            {
                num.negate();
                den.negate();
            }
            CHECK(holds(a / b, num, den));
        }
        auto const diff = fa.num * fb.den - fb.num * fa.den;
        CHECK(compare(a, b) == diff.sign());
        CHECK((a == b) == diff.is_zero());

        // the unreduced type reaches the same values once reduced
        auto const ua = bigrat_unreduced(a);
        auto const ub = bigrat_unreduced(b);
        CHECK((ua + ub).reduced() == a + b);
        CHECK((ua - ub).reduced() == a - b);
        CHECK((ua * ub).reduced() == a * b);
        CHECK((ua - ub).sign() == diff.sign());
    }
}

TEST("tg bigrat - operands may alias the result")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 1000; ++i)
    {
        auto const fa = random_fraction(rng, 3);
        auto const fb = random_fraction(rng, 3);
        auto const a = bigrat(fa.num, fa.den);
        auto const b = bigrat(fb.num, fb.den);
        auto x = a;
        x += b;
        CHECK(x == a + b);
        x = a;
        x.assign_sub(b, x);
        CHECK(x == b - a);
        x = a;
        x.assign_mul(x, x);
        CHECK(x == a * a);
        x = a;
        x *= b;
        CHECK(x == a * b);
        if (!b.is_zero())
        {
            x = a;
            x /= b;
            CHECK(x == a / b);
        }
        if (!a.is_zero())
        {
            x = a;
            x.assign_div(b, x);
            CHECK(x == b / a);
            x = a;
            x.assign_div(x, x);
            CHECK(x == bigrat(1));
        }
    }
}

TEST("tg bigrat - small values and their spelling")
{
    CHECK(bigrat(bigint(6), bigint(-4)).to_string() == "-3/2");
    CHECK(bigrat(bigint(6), bigint(3)).to_string() == "2");
    CHECK(bigrat(bigint(0), bigint(-7)).to_string() == "0");
    CHECK((bigrat(bigint(1), bigint(2)) + bigrat(bigint(1), bigint(2))).is_integer());
    CHECK((bigrat(bigint(1), bigint(3)) * bigrat(3)) == bigrat(1));
    CHECK(bigrat(bigint(1), bigint(3)) < bigrat(bigint(1), bigint(2)));
    CHECK(bigrat(bigint(-1), bigint(2)) < bigrat(bigint(-1), bigint(3)));
    CHECK(bigrat(bigint(2), bigint(3)).to_f64() == 2.0 / 3.0);
    CHECK(bigrat(bigint(-1), bigint(3)).to_f64() == -1.0 / 3.0);
}

TEST("tg bigrat - doubles convert exactly in and correctly rounded out")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 20000; ++i)
    {
        // any finite bit pattern, subnormals included
        auto const x = cc::bit_cast<f64>(rng.next_u64());
        if (!cc::is_finite(x))
            continue;
        auto const r = bigrat::from_f64(x);
        CHECK(r.to_f64() == x);
        CHECK(r.sign() == (x > 0 ? 1 : x < 0 ? -1 : 0));
        // a double's denominator is a power of two
        auto ones = 0;
        for (auto const l : r.denominator().limbs())
            ones += cc::popcount(l);
        CHECK(ones == 1);
    }
    // quotients of small integers round as the hardware's division does, both being correctly rounded
    for (auto i = 0; i < 20000; ++i)
    {
        auto const p = i64(rng.next_u64() >> 12) - (i64(1) << 51);
        auto const q = i64(rng.next_u64() >> 12) + 1;
        CHECK(bigrat(bigint(p), bigint(q)).to_f64() == f64(p) / f64(q));
    }
}
