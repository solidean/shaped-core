#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigfloat/bigfloat.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/scalar/float_expansion/float_expansion.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;
using tg::fi256;

static_assert(sizeof(tg::float_expansion_sbo<1>) == 16);
static_assert(sizeof(tg::float_expansion_sbo<2>) == 24);
static_assert(sizeof(tg::float_expansion_sbo<4>) == 40);

namespace
{
/// A double with its exponent in [-e, e], or a short integer, of either sign: products of up to degree 1000 / e stay in range.
f64 random_double(cc::random& rng, int e)
{
    auto const sign = (rng.next_u64() & 1) != 0 ? -1.0 : 1.0;
    if (rng.next_u64() % 8 == 0)
        return sign * f64(rng.next_u64() >> (11 + rng.next_u64() % 53));
    auto const x = int(rng.next_u64() % u64(2 * e + 1)) - e;
    return sign * tg::scale_by_pow2(f64(rng.next_u64() >> 11), x - 53);
}

/// The exact value, as the oracle holds it.
template <int K>
tg::bigfloat exact(tg::float_expansion_sbo<K> const& x)
{
    auto r = tg::bigfloat(0);
    for (auto c : x.components())
        r += tg::bigfloat(c);
    return r;
}

/// The representation's invariant: no zero, smallest magnitude first, and each component below the lowest set bit of the next.
template <int K>
bool is_nonoverlapping(tg::float_expansion_sbo<K> const& x)
{
    auto const c = x.components();
    for (auto i = 0; i < isize(c.size()); ++i)
    {
        if (c[i] == 0.0)
            return false;
        if (i + 1 < isize(c.size()))
        {
            auto const lowest_bit = tg::scale_by_pow2(1.0, tg::bigfloat(c[i + 1]).exponent());
            if (!(tg::abs(c[i]) < lowest_bit))
                return false;
        }
    }
    return true;
}

template <int K>
void check_against_bigfloat()
{
    using fe = tg::float_expansion_sbo<K>;
    using bf = tg::bigfloat;
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 50000 : 5000;
    for (auto i = 0; i < n; ++i)
    {
        // degree 4 over exponents within ±200 stays above the 2^-960 product floor
        f64 v[8];
        for (auto& x : v)
            x = random_double(rng, 200);
        auto const a = fe(v[0]) - fe(v[1]);
        auto const b = fe(v[2]) - fe(v[3]);
        auto const c = fe(v[4]) + fe(v[5]);
        auto const d = fe(v[6]) - fe(v[7]);
        auto const ea = bf(v[0]) - bf(v[1]);
        auto const eb = bf(v[2]) - bf(v[3]);
        auto const ec = bf(v[4]) + bf(v[5]);
        auto const ed = bf(v[6]) - bf(v[7]);

        auto const p = a * b - c * d;
        auto const q = p * (a + d) - b * c * d;
        CHECK(exact(a) == ea);
        CHECK(exact(a * b) == ea * eb);
        CHECK(exact(p) == ea * eb - ec * ed);
        CHECK(exact(q) == (ea * eb - ec * ed) * (ea + ed) - eb * ec * ed);
        CHECK(q.sign() == ((ea * eb - ec * ed) * (ea + ed) - eb * ec * ed).sign());
        CHECK(is_nonoverlapping(a));
        CHECK(is_nonoverlapping(p));
        CHECK(is_nonoverlapping(q));
        CHECK(compare(a, b) == compare(ea, eb));
    }
}

template <int K>
void check_ring_identities()
{
    using fe = tg::float_expansion_sbo<K>;
    auto rng = nx::test_random();
    for (auto i = 0; i < 5000; ++i)
    {
        auto const a = fe(random_double(rng, 300)) + fe(random_double(rng, 300));
        auto const b = fe(random_double(rng, 300)) - fe(random_double(rng, 300));
        auto const c = fe(random_double(rng, 100));

        CHECK((a + b) - a == b);
        CHECK(a - b == -(b - a));
        CHECK((a - a).is_zero());
        CHECK((a * b) * c == a * (b * c));
        CHECK(a * (b + c) == a * b + a * c);
        auto x = a;
        x *= b;
        x -= a * b;
        CHECK(x.is_zero());
    }
}

template <int K>
void check_integers()
{
    using fe = tg::float_expansion_sbo<K>;
    auto rng = nx::test_random();
    for (auto i = 0; i < 5000; ++i)
    {
        auto const v = i64(rng.next_u64());
        CHECK(exact(fe(v)) == tg::bigfloat(v));
        CHECK(is_nonoverlapping(fe(v)));
        CHECK(exact(fe(u64(v))) == tg::bigfloat(u64(v)));

        // orient2d over 62-bit integers in a 256-bit width, against the expansion's
        i64 c[6];
        for (auto& x : c)
            x = i64(rng.next_u64() >> 2) - (i64(1) << 61);
        auto const det = (fi256(c[0]) - fi256(c[4])) * (fi256(c[3]) - fi256(c[5]))
                       - (fi256(c[1]) - fi256(c[5])) * (fi256(c[2]) - fi256(c[4]));
        auto const e = (fe(c[0]) - fe(c[4])) * (fe(c[3]) - fe(c[5])) - (fe(c[1]) - fe(c[5])) * (fe(c[2]) - fe(c[4]));
        CHECK(e == fe::from_fixed_int(det));
        CHECK(exact(fe::from_fixed_int(det)) == tg::bigfloat::from_fixed_int(det));
        CHECK(e.sign() == det.sign());
    }
    CHECK(fe(i64(-1) << 63).sign() == -1);
    CHECK(exact(fe(i64(-1) << 63)) == tg::bigfloat(i64(-1) << 63));
}

template <int K>
void check_storage()
{
    using fe = tg::float_expansion_sbo<K>;

    // a double is one component, a difference or product of two at most two
    CHECK(fe(1.5).components().size() == 1);
    CHECK((fe(1.0) + fe(0x1p-80)).components().size() == 2);
    CHECK((fe(1.0 / 3.0) * fe(1.0 / 7.0)).components().size() == 2);
    CHECK(fe(-0.0).is_zero());
    CHECK(fe(0.0) == fe(-0.0));
    CHECK(fe(1.0) - fe(1.0) == fe(0.0));

    // a long expansion spills, and a copy and a move of it keep the value
    auto x = fe(1.0);
    for (auto i = 1; i <= 12; ++i)
        x += fe(tg::scale_by_pow2(1.0, -60 * i));
    CHECK(x.components().size() == 13);
    CHECK(!x.is_inline());
    auto const y = x;
    CHECK(y == x);
    auto z = cc::move(x);
    CHECK(z == y);
    CHECK(x.is_zero());
    // a spilled value reused for a short result keeps its heap and reads it
    z = fe(2.0) * fe(3.0);
    z += fe(1.0);
    CHECK(z == fe(7.0));
    CHECK(z.sign() == 1);
    CHECK((-z).sign() == -1);
    CHECK(z.approximate() == 7.0);
}
template <int K>
void check_invalid()
{
    using fe = tg::float_expansion_sbo<K>;
    // GCC refuses an overflow in a constant expression, so the infinity is made at run time
    auto const inf = 0x1p1023 * 2.0;
    auto const big = fe(0x1p1023) + fe(0x1p1023);
    auto const tiny = fe(0x1p-500) * fe(0x1p-500);
    auto const long_value = fe(1.0) + fe(0x1p-80) + fe(0x1p-200);

    // every way out of range: an overflowing sum, a product below 2^-960 or past 2^1024, a non-finite double, a huge integer
    CHECK(!big.is_valid());
    CHECK(!tiny.is_valid());
    CHECK(!(fe(0x1p600) * fe(0x1p600)).is_valid());
    CHECK(!(long_value * fe(0x1p-950)).is_valid());
    CHECK(!fe(inf).is_valid());
    CHECK(!fe(inf - inf).is_valid());
    u64 const limbs[17] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(!fe::from_limbs(cc::span<u64 const>(limbs, 17), false).is_valid());
    CHECK(fe::from_limbs(cc::span<u64 const>(limbs, 16), false).is_zero());

    // the edges of the range are still exact
    CHECK((fe(0x1p-480) * fe(0x1p-480)).is_valid());
    CHECK((fe(0x1p1022) + fe(0x1p1022)).is_valid());

    // sticky, like a NaN: through every operation, its operand order and a zero operand included
    for (auto const& x : {big, tiny})
    {
        CHECK(!(x + fe(1.0)).is_valid());
        CHECK(!(fe(1.0) - x).is_valid());
        CHECK(!(x * fe(0.0)).is_valid());
        CHECK(!(fe(0.0) * x).is_valid());
        CHECK(!(long_value * x).is_valid());
        CHECK(!(long_value + x).is_valid());
        CHECK(!(-x).is_valid());
        auto y = long_value;
        y *= x;
        CHECK(!y.is_valid());
        auto const copy = x;
        CHECK(!copy.is_valid());
        auto moved = fe(x);
        auto const target = cc::move(moved);
        CHECK(!target.is_valid());

        CHECK(x.sign() == fe::invalid_sign);
        CHECK(!x.is_zero());
        CHECK(!x.is_negative());
        CHECK(x.components().empty());
        CHECK(x.approximate() != x.approximate());
        CHECK(compare(x, fe(1.0)) == fe::invalid_sign);
        CHECK(!(x == x));
        CHECK((x <=> fe(1.0)) == std::partial_ordering::unordered);
        CHECK(!(x < fe(1.0)));
        CHECK(!(x > fe(1.0)));
    }
}
} // namespace

TEST("tg::float_expansion - a result out of range is invalid, and stays so like a NaN")
{
    check_invalid<1>();
    check_invalid<4>();
}

TEST("tg::float_expansion - exact against bigfloat, the components nonoverlapping")
{
    check_against_bigfloat<1>();
    check_against_bigfloat<4>();
}

TEST("tg::float_expansion - ring identities hold exactly")
{
    check_ring_identities<1>();
    check_ring_identities<4>();
}

TEST("tg::float_expansion - integers and fixed_int convert exactly")
{
    check_integers<1>();
    check_integers<4>();
}

TEST("tg::float_expansion - storage: inline, spilled, copied and moved")
{
    check_storage<1>();
    check_storage<4>();
}
