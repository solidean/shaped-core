#include <clean-core/math/float.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigfloat/bigfloat.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <cmath>

using namespace cc::primitive_defines;
using tg::fi256;

constexpr f64 f64_true_min = 0x1p-1074;
constexpr f64 f64_min_normal = 0x1p-1022;
constexpr f64 f64_max = 0x1.fffffffffffffp+1023;

static_assert(sizeof(tg::bigfloat_sbo<1>) == 16);
static_assert(sizeof(tg::bigfloat_sbo<2>) == 24);
static_assert(sizeof(tg::bigfloat_sbo<3>) == 32);

namespace
{
/// A finite double with its exponent in [min_exp, max_exp], or a short integer, of either sign.
/// The low end reaches the subnormals, which ldexp rounds into.
f64 random_double(cc::random& rng, int min_exp, int max_exp)
{
    auto const sign = (rng.next_u64() & 1) != 0 ? -1.0 : 1.0;
    if (rng.next_u64() % 8 == 0)
        return sign * f64(rng.next_u64() >> (11 + rng.next_u64() % 53));
    auto const e = min_exp + int(rng.next_u64() % u64(max_exp - min_exp + 1));
    return sign * tg::scale_by_pow2(f64(rng.next_u64() >> 11), e - 53);
}

/// A double that rounding an exact result into is correct: zero or normal, and finite.
bool is_normal_or_zero(f64 v)
{
    return v == 0.0 || (cc::is_finite(v) && tg::abs(v) >= f64_min_normal);
}

template <int K>
void check_against_the_hardware()
{
    using bf = tg::bigfloat_sbo<K>;
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 200000 : 20000;
    for (auto i = 0; i < n; ++i)
    {
        // wide spreads for sums, so a sum spills; products stay below overflow
        auto const a = random_double(rng, -1074, 1000);
        auto const b = random_double(rng, -1074, 1000);
        auto const x = random_double(rng, -500, 500);
        auto const y = random_double(rng, -500, 500);
        auto const z = random_double(rng, -1000, 1000);

        CHECK(bf(a).to_f64() == a);
        if (is_normal_or_zero(a + b))
            CHECK((bf(a) + bf(b)).to_f64() == a + b);
        if (is_normal_or_zero(a - b))
            CHECK((bf(a) - bf(b)).to_f64() == a - b);
        if (is_normal_or_zero(x * y))
            CHECK((bf(x) * bf(y)).to_f64() == x * y);
        if (is_normal_or_zero(std::fma(x, y, z)))
            CHECK(bf::msum({{bf(x), bf(y)}, {bf(z)}}).to_f64() == std::fma(x, y, z));
        CHECK(compare(bf(a), bf(b)) == (a < b ? -1 : a > b ? 1 : 0));
    }
}

template <int K>
void check_ring_identities()
{
    using bf = tg::bigfloat_sbo<K>;
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 50000 : 5000;
    for (auto i = 0; i < n; ++i)
    {
        auto const a = bf(random_double(rng, -1074, 1000));
        auto const b = bf(random_double(rng, -1074, 1000));
        auto const c = bf(random_double(rng, -300, 300));
        auto const d = bf(random_double(rng, -300, 300));

        CHECK((a + b) - a == b);
        CHECK(a - b == -(b - a));
        CHECK((a - a).is_zero());
        CHECK((a * c) * d == a * (c * d));
        CHECK(a * (c + d) == a * c + a * d);
        CHECK(bf::msum({{a, c}, {b, d, true}, {c}}) == a * c - b * d + c);
        CHECK(bf::msum({{a, c}, {a, c, true}}).is_zero());
        CHECK(compare(a, b) == (a - b).sign());
        CHECK(compare(a * c, b * d) == (a * c - b * d).sign());
    }
}

template <int K>
void check_against_fixed_int()
{
    using bf = tg::bigfloat_sbo<K>;
    auto rng = nx::test_random();
    auto const coord = [&rng] { return i64(rng.next_u64() >> 2) - (i64(1) << 61); };
    for (auto i = 0; i < 5000; ++i)
    {
        i64 v[6];
        for (auto& c : v)
            c = coord();
        // orient2d of (v0, v1), (v2, v3), (v4, v5): degree 2, within 128 bits
        auto const ex = fi256(v[0]) - fi256(v[4]);
        auto const ey = fi256(v[1]) - fi256(v[5]);
        auto const fx = fi256(v[2]) - fi256(v[4]);
        auto const fy = fi256(v[3]) - fi256(v[5]);
        auto const det = ex * fy - ey * fx;

        auto const bex = bf(v[0]) - bf(v[4]);
        auto const bey = bf(v[1]) - bf(v[5]);
        auto const bfx = bf(v[2]) - bf(v[4]);
        auto const bfy = bf(v[3]) - bf(v[5]);
        auto const expected = bf::from_fixed_int(det);
        CHECK(bex * bfy - bey * bfx == expected);
        CHECK(bf::msum({{bex, bfy}, {bey, bfx, true}}) == expected);
        CHECK(expected.sign() == det.sign());

        auto const shift = int(rng.next_u64() % 2000) - 1000;
        CHECK(bf::from_fixed_int(det, shift) == expected * bf(tg::scale_by_pow2(1.0, shift)));
    }
}

template <int K>
void check_storage()
{
    using bf = tg::bigfloat_sbo<K>;

    // a double is one limb, its trailing zeros moved into the exponent, so a product of two is at most two
    CHECK(bf(1.5).limbs().size() == 1);
    CHECK(bf(1.5).limbs()[0] == 3);
    CHECK((bf(1.0 / 3.0) * bf(1.0 / 7.0)).limbs().size() == 2);
    CHECK((bf(1.0 / 3.0) * bf(1.0 / 7.0)).is_inline() == (K >= 2));

    // negative zero is zero, with no sign and exponent 0
    CHECK(bf(-0.0).is_zero());
    CHECK(bf(-0.0).sign() == 0);
    CHECK(bf(-0.0).exponent() == 0);
    CHECK(bf(0.0) == bf(-0.0));

    // an exponent gap is paid in limbs: 2^1000 + 2^-1000 spans 2001 bits
    auto const wide = bf(tg::scale_by_pow2(1.0, 1000)) + bf(tg::scale_by_pow2(1.0, -1000));
    CHECK(wide.limbs().size() == 32);
    CHECK(!wide.is_inline());
    CHECK(wide - bf(tg::scale_by_pow2(1.0, 1000)) == bf(tg::scale_by_pow2(1.0, -1000)));

    auto copy = wide;
    CHECK(copy == wide);
    CHECK(copy.limbs().data() != wide.limbs().data());
    auto moved = cc::move(copy);
    CHECK(moved == wide);
    CHECK(copy.is_zero());
    CHECK(copy.is_inline());

    // a spilled value keeps its capacity, so a smaller result reuses it
    auto const* const before = moved.limbs().data();
    moved.assign_mul(bf(3.0), bf(5.0));
    CHECK(moved == bf(15.0));
    CHECK(moved.limbs().data() == before);
}

template <int K>
void check_aliasing()
{
    using bf = tg::bigfloat_sbo<K>;
    auto const big = bf(tg::scale_by_pow2(1.0, 600)) + bf(tg::scale_by_pow2(1.0, -600)); // spilled at every K

    auto a = big;
    a += a;
    CHECK(a == big * bf(2.0));
    a -= a;
    CHECK(a.is_zero());

    auto m = big;
    m *= m;
    CHECK(m == big * big);

    auto s = big;
    s.assign_msum({{s, s}, {s, true}});
    CHECK(s == big * big - big);

    auto t = bf(3.0);
    t.assign_add(t, big);
    CHECK(t == big + bf(3.0));
}
} // namespace

TEST("tg bigfloat - rounds to what the hardware computes")
{
    check_against_the_hardware<1>();
    check_against_the_hardware<2>();
    check_against_the_hardware<3>();
}

TEST("tg bigfloat - ring identities hold exactly")
{
    check_ring_identities<1>();
    check_ring_identities<2>();
    check_ring_identities<3>();
}

TEST("tg bigfloat - agrees with fixed_int")
{
    check_against_fixed_int<1>();
    check_against_fixed_int<2>();
    check_against_fixed_int<3>();
}

TEST("tg bigfloat - storage")
{
    check_storage<1>();
    check_storage<2>();
    check_storage<3>();
}

TEST("tg bigfloat - operands may alias the result")
{
    check_aliasing<1>();
    check_aliasing<2>();
    check_aliasing<3>();
}

TEST("tg bigfloat - every double converts back exactly")
{
    using bf = tg::bigfloat;
    CHECK(bf(f64_min_normal).to_f64() == f64_min_normal);
    CHECK(bf(f64_max).to_f64() == f64_max);
    CHECK(bf(f64_true_min).to_f64() == f64_true_min);
    CHECK(bf(-f64_true_min).to_f64() == -f64_true_min);
    CHECK(bf(i64(1) << 62).to_f64() == tg::scale_by_pow2(1.0, 62));
    CHECK(bf(u64(-1)).to_f64() == tg::scale_by_pow2(1.0, 64)); // 2^64 - 1 rounds up
    CHECK(bf(i64(-7)).sign() == -1);

    // products past the double range stay exact, with the i32 exponent
    auto const tiny = bf(f64_true_min) * bf(f64_true_min) * bf(f64_true_min);
    CHECK(tiny.exponent() == -3 * 1074);
    CHECK(tiny.sign() == 1);
    CHECK(compare(tiny, bf(0.0)) == 1);
    auto const huge = bf(f64_max) * bf(f64_max);
    CHECK(huge > bf(f64_max));
    CHECK(!cc::is_finite(huge.to_f64()));
    CHECK(huge.to_f64() > 0.0);
}
