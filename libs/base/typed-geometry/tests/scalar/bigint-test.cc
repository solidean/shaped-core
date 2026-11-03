#include <clean-core/container/fixed_array.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;
using tg::bigint;
using tg::fi256;

static_assert(sizeof(bigint) == sizeof(void*));

namespace
{
/// A limb that is all ones, zero or one as often as random, so carries and borrows run the whole length.
u64 random_limb(cc::random& rng)
{
    switch (rng.next_u64() % 8)
    {
    case 0:
        return 0;
    case 1:
        return ~u64(0);
    case 2:
        return 1;
    case 3:
        return u64(1) << 63;
    default:
        return rng.next_u64();
    }
}

/// Up to max_limbs limbs of either sign, zero included.
bigint random_bigint(cc::random& rng, int max_limbs)
{
    u64 limbs[64];
    auto const n = int(rng.next_u64() % u64(max_limbs + 1));
    for (auto i = 0; i < n; ++i)
        limbs[i] = random_limb(rng);
    return bigint::from_limbs(cc::span<u64 const>(limbs, isize(n)), (rng.next_u64() & 1) != 0);
}

/// x's value below 2^bits in magnitude, of either sign.
fi256 random_fi256(cc::random& rng, int bits)
{
    fi256 x;
    for (auto& l : x.limbs)
        l = random_limb(rng);
    x = (x << (256 - bits)) >> (256 - bits); // arithmetic, so the sign is spread over the top
    return x;
}

/// floor(a / 2^n), through the truncating division.
bigint floor_div_pow2(bigint const& a, i64 n)
{
    auto const [q, r] = div_mod(a, bigint(1) << n);
    return r.is_negative() ? q - bigint(1) : q;
}
} // namespace

TEST("tg bigint - agrees with fixed_int")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 100000 : 10000;
    for (auto i = 0; i < n; ++i)
    {
        // products of operands below 2^126 stay below 2^252, so fi256 holds every result exactly
        auto const x = random_fi256(rng, 1 + int(rng.next_u64() % 126));
        auto const y = random_fi256(rng, 1 + int(rng.next_u64() % 126));
        auto const a = bigint::from_fixed_int(x);
        auto const b = bigint::from_fixed_int(y);

        CHECK(a + b == bigint::from_fixed_int(x + y));
        CHECK(a - b == bigint::from_fixed_int(x - y));
        CHECK(a * b == bigint::from_fixed_int(x * y));
        CHECK(a.sign() == x.sign());
        CHECK(compare(a, b) == (x < y ? -1 : y < x ? 1 : 0));
        if (y.sign() != 0)
        {
            auto const [q, r] = div_mod(a, b);
            CHECK(q == bigint::from_fixed_int(x / y));
            CHECK(r == bigint::from_fixed_int(x % y));
        }
        auto const s = int(rng.next_u64() % 120);
        CHECK((a << s) == bigint::from_fixed_int(x << s));
        CHECK((a >> s) == bigint::from_fixed_int(x >> s));
    }
}

TEST("tg bigint - agrees with i64")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 20000; ++i)
    {
        // within i32, so every product fits
        auto const x = i64(i32(rng.next_u64()));
        auto const y = i64(i32(rng.next_u64() >> (rng.next_u64() % 32)));
        CHECK(bigint(x) + bigint(y) == bigint(x + y));
        CHECK(bigint(x) - bigint(y) == bigint(x - y));
        CHECK(bigint(x) * bigint(y) == bigint(x * y));
        CHECK((bigint(x) * bigint(y)).to_i64() == x * y);
        if (y != 0)
        {
            CHECK(bigint(x) / bigint(y) == bigint(x / y));
            CHECK(bigint(x) % bigint(y) == bigint(x % y));
        }
        // within 31 bits, so x * 2^s fits
        auto const s = int(rng.next_u64() % 32);
        CHECK((bigint(x) >> s) == bigint(x >> s));
        CHECK((bigint(x) << s) == bigint(x * (i64(1) << s)));
        CHECK(bigint(x).to_f64() == f64(x));
    }
}

TEST("tg bigint - ring and division identities hold exactly")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 20000 : 2000;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = random_bigint(rng, 40);
        auto const b = random_bigint(rng, 40);
        auto const c = random_bigint(rng, 12);

        CHECK((a + b) - b == a);
        CHECK(a - b == -(b - a));
        CHECK((a - a).is_zero());
        CHECK(a * (b + c) == a * b + a * c);
        CHECK((a * b) * c == a * (b * c));
        CHECK(compare(a, b) == (a - b).sign());

        if (!b.is_zero())
        {
            auto const [q, r] = div_mod(a, b);
            CHECK(q * b + r == a);
            CHECK(compare(abs(r), abs(b)) < 0);
            CHECK((r.is_zero() || r.sign() == a.sign()));
            CHECK((a * b) / b == a);
            CHECK(((a * b) % b).is_zero());
        }

        auto const s = i64(rng.next_u64() % 3000);
        CHECK(((a << s) >> s) == a);
        CHECK((a >> s) == floor_div_pow2(a, s));
        CHECK((a << s) == a * (bigint(1) << s));
    }
}

TEST("tg bigint - every sum shape agrees with limb-at-a-time sums")
{
    // up to 20 limbs each, so the kernel's first loop meets every length mod 4 and its tail every length,
    // into a target that holds the result, the lean path, and in place
    auto rng = nx::test_random();
    auto r = bigint(1) << (64 * 40); // a node with room for every result
    for (auto n = 1; n <= 20; ++n)
        for (auto m = 1; m <= 20; ++m)
        {
            // all ones: 2^64n - 1 + 2^64m - 1, and their difference, where every carry and borrow runs as far as it can
            auto const ones_n = (bigint(1) << (64 * n)) - bigint(1);
            auto const ones_m = (bigint(1) << (64 * m)) - bigint(1);
            r.assign_add(ones_n, ones_m);
            CHECK(r == (bigint(1) << (64 * n)) + (bigint(1) << (64 * m)) - bigint(2));
            r.assign_sub(ones_n, ones_m);
            CHECK(r == (bigint(1) << (64 * n)) - (bigint(1) << (64 * m)));

            u64 a_limbs[20];
            u64 b_limbs[20];
            for (auto i = 0; i < n; ++i)
                a_limbs[i] = random_limb(rng);
            for (auto i = 0; i < m; ++i)
                b_limbs[i] = random_limb(rng);
            a_limbs[n - 1] |= 1;
            b_limbs[m - 1] |= 1;
            auto const a = bigint::from_limbs(cc::span<u64 const>(a_limbs, isize(n)), (rng.next_u64() & 1) != 0);
            auto const b = bigint::from_limbs(cc::span<u64 const>(b_limbs, isize(m)), (rng.next_u64() & 1) != 0);

            // b one limb at a time, each sum of a different shape than a + b
            auto sum = a;
            auto difference = a;
            for (auto i = 0; i < m; ++i)
            {
                auto term = bigint(b_limbs[i]) << (64 * i);
                if (b.is_negative())
                    term.negate();
                sum += term;
                difference -= term;
            }

            r.assign_add(a, b);
            CHECK(r == sum);
            r.assign_sub(a, b);
            CHECK(r == difference);
            auto x = a;
            x += b;
            CHECK(x == sum);
            x = a;
            x -= b;
            CHECK(x == difference);
            auto y = b;
            y.assign_add(a, y);
            CHECK(y == sum);
            y = b;
            y.assign_sub(a, y);
            CHECK(y == difference);
        }
}

TEST("tg bigint - every product shape agrees with the one-limb rows")
{
    // up to 70 limbs, past the fixed kernels, Karatsuba's threshold, its odd splits and the chunks of an unbalanced product
    auto rng = nx::test_random();
    for (auto n = 1; n <= 70; ++n)
        for (auto m = 1; m <= n; m += (m < 20 ? 1 : 7))
        {
            // all ones: (2^64n - 1)(2^64m - 1), where every carry runs as far as it can
            auto const ones_n = (bigint(1) << (64 * n)) - bigint(1);
            auto const ones_m = (bigint(1) << (64 * m)) - bigint(1);
            CHECK(ones_n * ones_m
                  == (bigint(1) << (64 * (n + m))) - (bigint(1) << (64 * n)) - (bigint(1) << (64 * m)) + bigint(1));

            // random: the sum of a times each limb of b, shifted, never takes a path past one limb
            u64 a_limbs[70];
            for (auto i = 0; i < n; ++i)
                a_limbs[i] = random_limb(rng);
            a_limbs[n - 1] |= 1;
            auto const a = bigint::from_limbs(cc::span<u64 const>(a_limbs, isize(n)), (rng.next_u64() & 1) != 0);
            u64 limbs[70];
            for (auto i = 0; i < m; ++i)
                limbs[i] = random_limb(rng);
            limbs[m - 1] |= 1;
            auto const b = bigint::from_limbs(cc::span<u64 const>(limbs, isize(m)));
            auto expected = bigint();
            for (auto i = 0; i < m; ++i)
                expected += (abs(a) * bigint(limbs[i])) << (64 * i);
            if (a.is_negative())
                expected.negate();
            CHECK(a * b == expected);
            CHECK(b * a == expected);
        }
}

TEST("tg bigint - assign_add_mul and assign_sub_mul agree with the product and the sum")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 50000 : 5000;
    for (auto i = 0; i < n; ++i)
    {
        // one-limb factors and small accumulators most of the time, the inline path's shapes, with their boundaries
        auto const a = random_bigint(rng, rng.next_u64() % 4 == 0 ? 3 : 1);
        auto const b = random_bigint(rng, rng.next_u64() % 4 == 0 ? 3 : 1);
        auto const acc = random_bigint(rng, 1 + int(rng.next_u64() % 4));

        auto r = acc;
        r.assign_add_mul(a, b);
        CHECK(r == acc + a * b);
        r = acc;
        r.assign_sub_mul(a, b);
        CHECK(r == acc - a * b);

        // the accumulator may be either factor, or both
        r = acc;
        r.assign_add_mul(r, b);
        CHECK(r == acc + acc * b);
        r = acc;
        r.assign_sub_mul(a, r);
        CHECK(r == acc - a * acc);
        r = acc;
        r.assign_add_mul(r, r);
        CHECK(r == acc + acc * acc);
    }

    // a product cancelling the accumulator exactly leaves zero
    auto z = bigint(-6);
    z.assign_add_mul(bigint(2), bigint(3));
    CHECK(z.is_zero());
    z.assign_sub_mul(bigint(0), bigint(5));
    CHECK(z.is_zero());
}

TEST("tg bigint - an accumulator's sum of products agrees with the bigint sum")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 20000 : 3000;
    auto const check_width = [&]<int W>()
    {
        for (auto i = 0; i < n; ++i)
        {
            tg::bigint_accumulator<W> acc;
            bigint expected;
            auto const terms = 1 + int(rng.next_u64() % 12);
            for (auto t = 0; t < terms; ++t)
            {
                auto const a = random_bigint(rng, tg::bigint_accumulator<W>::factor_limbs);
                auto const b = random_bigint(rng, tg::bigint_accumulator<W>::factor_limbs);
                acc.add_mul(a, b);
                expected.assign_add_mul(a, b);
                // now and then the negated product too, so sums cancel back through zero
                if (rng.next_u64() % 5 == 0)
                {
                    acc.add_mul(-a, b);
                    expected.assign_sub_mul(a, b);
                }
            }
            CHECK(acc.to_bigint() == expected);
            CHECK(acc.is_zero() == expected.is_zero());
            acc.clear();
            CHECK(acc.is_zero());
        }
    };
    check_width.template operator()<3>();
    check_width.template operator()<5>();

    // the signed word accumulator, over factors that fit an i64, its extremes included
    auto const extremes = cc::fixed_array<i64, 6>{
        {i64(1) << 62, -(i64(1) << 62), i64(0x7fffffffffffffff), i64(-0x7fffffffffffffff) - 1, -1, 0}};
    for (auto i = 0; i < n; ++i)
    {
        tg::bigint_word_accumulator acc;
        bigint expected;
        auto const terms = 1 + int(rng.next_u64() % 12);
        for (auto t = 0; t < terms; ++t)
        {
            auto const pick
                = [&] { return rng.next_u64() % 3 == 0 ? extremes[rng.next_u64() % 6] : i64(rng.next_u64()); };
            auto const a = bigint(pick());
            auto const b = bigint(pick());
            acc.add_mul(tg::bigint_word_accumulator::make_factor(a), tg::bigint_word_accumulator::make_factor(b));
            expected.assign_add_mul(a, b);
        }
        CHECK(acc.to_bigint() == expected);
    }
}

TEST("tg bigint - division recovers a known quotient and remainder")
{
    // divisors of all ones or a lone top bit, quotient limbs of all ones: windows whose top two limbs equal the divisor's,
    // and digits whose 3/2 estimate is one too large, the division's rarely taken branches
    auto rng = nx::test_random();
    for (auto m = 2; m <= 12; ++m)
        for (auto k = 1; k <= 12; ++k)
            for (auto round = 0; round < 8; ++round)
            {
                u64 d_limbs[12];
                for (auto i = 0; i < m; ++i)
                    d_limbs[i] = round % 2 == 0 ? random_limb(rng) : ~u64(0);
                d_limbs[m - 1] |= round % 4 < 2 ? u64(1) << 63 : u64(1);
                auto const d = bigint::from_limbs(cc::span<u64 const>(d_limbs, isize(m)));
                u64 q_limbs[12];
                for (auto i = 0; i < k; ++i)
                    q_limbs[i] = round < 4 ? ~u64(0) : random_limb(rng);
                q_limbs[k - 1] |= 1;
                auto const q = bigint::from_limbs(cc::span<u64 const>(q_limbs, isize(k)));
                // the largest remainder, or a random one below d
                auto const r = round % 3 == 0 ? d - bigint(1) : abs(random_bigint(rng, m)) % d;
                auto const a = q * d + r;
                auto const [qq, rr] = div_mod(a, d);
                CHECK(qq == q);
                CHECK(rr == r);
            }
}

TEST("tg bigint - gcd")
{
    CHECK(gcd(bigint(12), bigint(18)) == bigint(6));
    CHECK(gcd(bigint(-4), bigint(6)) == bigint(2));
    CHECK(gcd(bigint(0), bigint(-7)) == bigint(7));
    CHECK(gcd(bigint(0), bigint(0)).is_zero());

    auto rng = nx::test_random();
    for (auto i = 0; i < 500; ++i)
    {
        auto const a = random_bigint(rng, 10);
        auto const b = random_bigint(rng, 10);
        auto const c = random_bigint(rng, 6);
        auto const g = gcd(a * c, b * c);
        CHECK(!g.is_negative());
        if (!g.is_zero())
        {
            CHECK(((a * c) % g).is_zero());
            CHECK(((b * c) % g).is_zero());
            CHECK((c.is_zero() || (g % abs(c)).is_zero()));
        }
    }
}

TEST("tg bigint - gcd agrees with Euclid's at every size, and div_exact undoes a product")
{
    // Euclid on div_mod, the reference: slow, but nothing of the binary or Lehmer paths
    auto const euclid = [](bigint x, bigint y)
    {
        x = abs(x);
        y = abs(y);
        while (!y.is_zero())
        {
            auto r = x % y;
            x = cc::move(y);
            y = cc::move(r);
        }
        return x;
    };
    auto rng = nx::test_random();
    auto const random_magnitude = [&](int n)
    {
        u64 limbs[24];
        for (auto i = 0; i < n; ++i)
            limbs[i] = random_limb(rng);
        limbs[n - 1] |= 1;
        return bigint::from_limbs(cc::span<u64 const>(limbs, isize(n)));
    };
    for (auto n = 1; n <= 16; ++n)
        for (auto m = 1; m <= n; ++m)
            for (auto k = 0; k <= 4; ++k)
            {
                // a common factor of k limbs, none for k = 0, times a power of two now and then
                auto factor = k == 0 ? bigint(1) : random_magnitude(k);
                if (rng.next_u64() % 3 == 0)
                    factor <<= i64(rng.next_u64() % 130);
                auto const a = random_magnitude(n) * factor;
                auto const b = (rng.next_u64() % 2 == 0 ? -random_magnitude(m) : random_magnitude(m)) * factor;
                auto const expected = euclid(a, b);
                CHECK(gcd(a, b) == expected);
                CHECK(gcd(b, a) == expected);
                auto g = a;
                gcd(g, g, b);
                CHECK(g == expected);
                CHECK(div_exact(a, expected) * expected == a);
                CHECK(div_exact(b, -expected) * -expected == b);
                auto q = b;
                div_exact(q, q, expected);
                CHECK(q * expected == b);
            }
    // values whose Euclid quotients are all one, Fibonacci numbers, take the most steps there are
    auto f0 = bigint(1);
    auto f1 = bigint(1);
    for (auto i = 0; i < 700; ++i)
    {
        auto f2 = f0 + f1;
        f0 = cc::move(f1);
        f1 = cc::move(f2);
        CHECK(gcd(f1, f0) == bigint(1));
        CHECK(gcd(f1 * bigint(6), f0 * bigint(4)) == bigint(2) * gcd(f1 * bigint(3), f0 * bigint(2)));
    }
}

TEST("tg bigint - decimal strings")
{
    CHECK(bigint().to_string() == "0");
    CHECK(bigint(-42).to_string() == "-42");
    CHECK((bigint(1) << 64).to_string() == "18446744073709551616");
    CHECK(bigint(i64(-9223372036854775807ll - 1)).to_string() == "-9223372036854775808");
    CHECK(bigint::parse("100000000000000000000000000000000000000000").value()
          == bigint::parse("10000000000000000000").value() * bigint::parse("10000000000000000000000").value());
    CHECK(bigint::parse("-0").value().is_zero());
    CHECK(bigint::parse("+17").value() == bigint(17));
    CHECK(!bigint::parse("").has_value());
    CHECK(!bigint::parse("-").has_value());
    CHECK(!bigint::parse("12a").has_value());

    auto rng = nx::test_random();
    for (auto i = 0; i < 2000; ++i)
    {
        auto const a = random_bigint(rng, 30);
        CHECK(bigint::parse(a.to_string()).value() == a);
    }
}

TEST("tg bigint - conversions")
{
    CHECK(bigint(i64(-9223372036854775807ll - 1)).fits_i64());
    CHECK(bigint(i64(-9223372036854775807ll - 1)).to_i64() == -9223372036854775807ll - 1);
    CHECK(!bigint(u64(1) << 63).fits_i64());
    CHECK(!(bigint(1) << 64).fits_i64());
    CHECK((bigint(1) << 1000).to_f64() == tg::scale_by_pow2(1.0, 1000));
    CHECK((-(bigint(1) << 1000)).to_f64() == -tg::scale_by_pow2(1.0, 1000));
    CHECK(bigint(u64(-1)).to_f64() == tg::scale_by_pow2(1.0, 64)); // 2^64 - 1 rounds up
    CHECK((bigint(1) << 1100).to_f64() > 1e308);

    auto const lowest = fi256(1) << 255;
    CHECK(bigint::from_fixed_int(lowest) == -(bigint(1) << 255));
    CHECK(bigint::from_fixed_int(tg::fu256(lowest)) == (bigint(1) << 255));
    CHECK(bigint(1).bit_width() == 1);
    CHECK((bigint(1) << 200).bit_width() == 201);
    CHECK(bigint().bit_width() == 0);
}

TEST("tg bigint - storage")
{
    // zero holds no node; every other value holds a node of a power-of-two class, behind a header word
    CHECK(bigint().capacity() == 0);
    CHECK(bigint(0).capacity() == 0);
    CHECK(bigint(5).capacity() == 1);
    CHECK((bigint(5) << 64).capacity() == 3);
    CHECK((bigint(5) << 192).capacity() == 7);
    CHECK((bigint(5) << 448).capacity() == 15);

    // a value keeps its node, so a smaller result reuses it
    auto x = bigint(1) << 400;
    auto const* const before = x.limbs().data();
    x.assign_mul(bigint(3), bigint(5));
    CHECK(x == bigint(15));
    CHECK(x.limbs().data() == before);
    x -= bigint(15);
    CHECK(x.is_zero());
    CHECK(x.capacity() == 7);
    x += bigint(2);
    CHECK(x.limbs().data() == before);

    // a sum grows by a limb only when its top limb carries
    auto y = bigint(u64(-1));
    CHECK(y.capacity() == 1);
    y += bigint(1);
    CHECK(y == (bigint(1) << 64));

    // past 65535 limbs the header's capacity saturates: the lean paths step aside, and the node is still reused
    auto huge = bigint(1) << (64 * 70000);
    CHECK(huge.capacity() == 131071);
    auto const* const huge_limbs = huge.limbs().data();
    huge += bigint(1);
    huge -= bigint(2);
    CHECK(huge == (bigint(1) << (64 * 70000)) - bigint(1));
    CHECK(huge.limbs().data() == huge_limbs);
    huge.assign_add(huge, huge);
    CHECK(huge == (bigint(1) << (64 * 70000 + 1)) - bigint(2));
    CHECK(huge.limbs().data() == huge_limbs);

    auto copy = x;
    CHECK(copy == x);
    CHECK(copy.limbs().data() != x.limbs().data());
    auto moved = cc::move(copy);
    CHECK(moved == x);
    CHECK(copy.is_zero());
    CHECK(copy.capacity() == 0);
}

TEST("tg bigint - operands may alias the result")
{
    auto const big = (bigint(1) << 600) + bigint(12345);

    auto a = big;
    a += a;
    CHECK(a == big * bigint(2));
    a -= a;
    CHECK(a.is_zero());

    auto m = big;
    m *= m;
    CHECK(m == big * big);

    // a product into one of its operands, holding the room or not
    auto p = big * big * big;
    auto const p_expected = p * big;
    p.assign_mul(p, big);
    CHECK(p == p_expected);
    auto q = big;
    q.assign_mul(big, q);
    CHECK(q == big * big);

    auto s = big;
    s <<= 1000;
    CHECK(s == big * (bigint(1) << 1000));
    s >>= 1000;
    CHECK(s == big);

    auto t = bigint(3);
    t.assign_add(t, big);
    CHECK(t == big + bigint(3));
    t.assign_sub(big, t);
    CHECK(t == bigint(-3));

    // a division into kept results, and into results that are its operands
    auto const big_q = div_mod(big * big + bigint(7), big);
    bigint dq;
    bigint dr;
    div_mod(dq, dr, big * big + bigint(7), big);
    CHECK(dq == big_q.quotient);
    CHECK(dr == big_q.remainder);
    auto const* const dq_node = dq.limbs().data();
    div_mod(dq, dr, big * bigint(3) + bigint(1), big);
    CHECK(dq == bigint(3));
    CHECK(dr == bigint(1));
    CHECK(dq.limbs().data() == dq_node);
    auto x = big * big + bigint(7);
    auto y = big;
    div_mod(x, y, x, y);
    CHECK(x == big_q.quotient);
    CHECK(y == big_q.remainder);

    // an rvalue operand lends its node to the result
    auto u = big;
    auto const* const node = u.limbs().data();
    auto v = cc::move(u) - big;
    CHECK(v.is_zero());
    v = big;
    CHECK(v.limbs().data() == node);
}
