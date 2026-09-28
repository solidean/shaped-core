#include "../approx.hh"

#include <clean-core/common/hash.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <nexus/test.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::f16;

// The references below are written independently of the kernels: a value is read off the bit fields in f64, and
// narrowing is a nearest-value search over the sorted halves.
// Every f16 value, every midpoint between two of them and every f16 product is exact in an f64.

static_assert(sizeof(f16) == 2 && alignof(f16) == 2);
static_assert(std::is_trivially_copyable_v<f16>);

// Conversions are explicit both ways, so a mixed expression is a compile error rather than a silent promotion.
static_assert(!std::is_convertible_v<f32, f16> && std::is_constructible_v<f16, f32>);
static_assert(!std::is_convertible_v<f16, f32> && std::is_constructible_v<f32, f16>);
static_assert(!std::is_convertible_v<int, f16> && std::is_constructible_v<f16, int>);
static_assert(!std::is_constructible_v<f16, bool>);
static_assert(!std::is_constructible_v<f16, char>);
template <class A, class B>
concept addable = requires(A a, B b) { a + b; };
static_assert(addable<f16, f16>);
static_assert(!addable<f16, f32> && !addable<f32, f16>);

// Everything the conversion does is available in constant evaluation.
static_assert(f16(0.5f).bits() == 0x3800);
static_assert(f16(0.1).bits() == 0x2e66);
static_assert(f16(3).bits() == 0x4200);
static_assert(f16::make_from_bits(0x3555).to_f32() == 0.333251953125f);
static_assert(f16(1.0f) + f16(2.0f) == f16(3.0f));
static_assert(tg::literals::operator""_f16(0.5L) == f16(0.5f));
static_assert(f16::max.bits() == 0x7bff && f16::infinity.is_inf() && f16::quiet_nan.is_nan());

static_assert(tg::traits::has_sqrt<f16> && tg::traits::has_trigonometry<f16> && tg::traits::has_exponential<f16>);
static_assert(tg::traits::has_rounding<f16> && tg::traits::has_abs<f16> && tg::traits::has_pow2<f16>);

namespace
{
constexpr u16 max_finite_bits = 0x7bff;

f64 reference_value(u16 h)
{
    auto const sign = (h & 0x8000) != 0 ? -1.0 : 1.0;
    auto const exponent = int((h >> 10) & 0x1f);
    auto const fraction = int(h & 0x03ff);
    if (exponent == 0)
        return sign * tg::scale_by_pow2(f64(fraction), -24);
    return sign * tg::scale_by_pow2(f64(1024 + fraction), exponent - 25);
}

/// Round to nearest, ties to even, of a finite f64, by searching the sorted non-negative halves.
u16 reference_narrow(f64 x)
{
    auto const sign = u16((cc::bit_cast<u64>(x) >> 48) & 0x8000);
    auto const a = x < 0 ? -x : x;
    if (a >= 65520.0) // the midpoint between the largest half and 2^16, and ties go to the even infinity
        return u16(sign | 0x7c00);

    // the largest h whose value is <= a
    auto lo = 0;
    auto hi = int(max_finite_bits);
    while (lo < hi)
    {
        auto const mid = (lo + hi + 1) / 2;
        if (reference_value(u16(mid)) <= a)
            lo = mid;
        else
            hi = mid - 1;
    }

    auto const below = lo;
    if (below == int(max_finite_bits) || reference_value(u16(below)) == a)
        return u16(sign | below);
    auto const above = below + 1;
    auto const midpoint = (reference_value(u16(below)) + reference_value(u16(above))) / 2;
    if (a < midpoint)
        return u16(sign | below);
    if (a > midpoint)
        return u16(sign | above);
    return u16(sign | ((below & 1) == 0 ? below : above));
}

u32 f32_bits(f32 v)
{
    return cc::bit_cast<u32>(v);
}

/// Whether r is the result `expected` names, reading any two NaNs as the same result.
bool same_result(f16 r, u16 expected)
{
    if (tg::impl::half_bits_are_nan(expected))
        return r.is_nan();
    return r.bits() == expected;
}

/// The f16 an f32 or f64 reference result narrows to.
u16 expected_from(f64 r)
{
    if (r != r)
        return 0x7e00;
    if (r > 65504.0 * 2 || r < -65504.0 * 2)
        return r > 0 ? u16(0x7c00) : u16(0xfc00);
    return reference_narrow(r);
}

u16 random_half(cc::random& rng)
{
    return u16(rng.next_u32());
}
} // namespace

TEST("tg f16 - widening is exact for every value")
{
    for (auto i = 0; i < 65536; ++i)
    {
        auto const h = u16(i);
        auto const wide = f16::make_from_bits(h).to_f32();
        auto const portable = tg::impl::half_bits_to_f32_portable(h);
        CHECK(f32_bits(wide) == f32_bits(portable));

        if (tg::impl::half_bits_are_nan(h))
        {
            // quiet, same sign, and the payload moves up intact
            CHECK(wide != wide);
            CHECK((f32_bits(wide) & 0x0040'0000) != 0);
            CHECK((f32_bits(wide) >> 31) == u32(h >> 15));
            CHECK(((f32_bits(wide) >> 13) & 0x1ff) == u32(h & 0x1ff));
        }
        else if ((h & 0x7fff) == 0x7c00)
            CHECK(f32_bits(wide) == ((u32(h & 0x8000) << 16) | 0x7f80'0000));
        else
        {
            CHECK(f64(wide) == reference_value(h));
            CHECK((f32_bits(wide) >> 31) == u32(h >> 15)); // -0 stays -0
        }
    }
}

TEST("tg f16 - narrowing rounds to nearest even at every midpoint")
{
    auto const check_f32 = [](f32 x)
    {
        auto const expected = reference_narrow(f64(x));
        CHECK(f16(x).bits() == expected);
        CHECK(tg::impl::f32_to_half_bits_portable(x) == expected);
        CHECK(f16(f64(x)).bits() == expected);
    };
    auto const check_f64 = [](f64 x) { CHECK(f16(x).bits() == reference_narrow(x)); };

    for (auto i = 0; i <= int(max_finite_bits); ++i)
    {
        auto const value = reference_value(u16(i));
        auto const next = i == int(max_finite_bits) ? 65536.0 : reference_value(u16(i + 1));
        auto const midpoint = (value + next) / 2;

        for (auto const x : {value, midpoint})
        {
            for (auto const sign : {1.0, -1.0})
            {
                auto const s = f32(sign * x); // exact: at most twelve significant bits
                check_f32(s);
                if (s != 0)
                {
                    check_f32(cc::bit_cast<f32>(f32_bits(s) - 1));
                    check_f32(cc::bit_cast<f32>(f32_bits(s) + 1));
                }
                auto const d = sign * x;
                check_f64(d);
                if (d != 0)
                {
                    check_f64(cc::bit_cast<f64>(cc::bit_cast<u64>(d) - 1));
                    check_f64(cc::bit_cast<f64>(cc::bit_cast<u64>(d) + 1));
                }
            }
        }
    }
}

TEST("tg f16 - narrowing random floats")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 200'000; ++i)
    {
        auto const x = cc::bit_cast<f32>(rng.next_u32());
        if (x != x || x - x != 0) // NaN and infinity have their own test
            continue;
        CHECK(f16(x).bits() == reference_narrow(f64(x)));
    }
}

TEST("tg f16 - the f32 and f64 narrowings agree on every f32", thorough_only)
{
    auto mismatches = 0;
    for (u64 i = 0; i < (u64(1) << 32); ++i)
    {
        auto const x = cc::bit_cast<f32>(u32(i));
        mismatches += tg::impl::f32_to_half_bits_portable(x) != tg::impl::f64_to_half_bits_portable(f64(x)) ? 1 : 0;
    }
    CHECK(mismatches == 0);
}

TEST("tg f16 - narrowing special values")
{
    SECTION("overflow reaches infinity at the midpoint above the largest half")
    {
        CHECK(f16(65504.0f) == f16::max);
        CHECK(f16(65519.99f) == f16::max);
        CHECK(f16(65520.0f).bits() == 0x7c00);
        CHECK(f16(-1e30f).bits() == 0xfc00);
        CHECK(f16(1e300).bits() == 0x7c00);
    }

    SECTION("a NaN stays a NaN, quiet, with its sign and the top of its payload")
    {
        CHECK(f16(cc::bit_cast<f32>(u32(0x7f80'0001))).bits() == 0x7e00); // signaling, payload below the kept bits
        CHECK(f16(cc::bit_cast<f32>(u32(0x7fc0'2000))).bits() == 0x7e01);
        CHECK(f16(cc::bit_cast<f32>(u32(0xffc0'0000))).bits() == 0xfe00);
        CHECK(f16(cc::bit_cast<f64>(u64(0x7ff8'0400'0000'0000))).bits() == 0x7e01);
    }

    SECTION("f64 rounds once where going through f32 would round twice")
    {
        auto const x = 1.0 + tg::scale_by_pow2(1.0, -11) + tg::scale_by_pow2(1.0, -30);
        CHECK(f16(x).bits() == 0x3c01);
        CHECK(f16(f32(x)).bits() == 0x3c00); // the double rounding the direct path avoids
    }

    SECTION("integers round once, ties to even")
    {
        CHECK(f16(2048).bits() == 0x6800);
        CHECK(f16(2049).bits() == 0x6800);
        CHECK(f16(2051).bits() == 0x6802);
        CHECK(f16(-3).bits() == 0xc200);
        CHECK(f16(70000).is_inf());
        CHECK(f16(~u64(0)).is_inf());
        CHECK(f16(u8(255)).to_f32() == 255.0f);
    }

    SECTION("the zeros keep their sign, and the smallest subnormal is 2^-24")
    {
        CHECK(f16(-0.0f).bits() == 0x8000);
        CHECK(f16(tg::scale_by_pow2(1.0f, -24)) == f16::denorm_min);
        CHECK(f16(tg::scale_by_pow2(1.0f, -25)).bits() == 0x0000);   // a tie, to the even zero
        CHECK(f16(tg::scale_by_pow2(1.5f, -25)) == f16::denorm_min); // above the tie
        CHECK(f16::min_normal.to_f32() == tg::scale_by_pow2(1.0f, -14));
        CHECK(f16::epsilon.to_f32() == tg::scale_by_pow2(1.0f, -10));
    }
}

TEST("tg f16 - classification")
{
    CHECK(f16::quiet_nan.is_nan());
    CHECK(!f16::infinity.is_nan());
    CHECK(f16::infinity.is_inf());
    CHECK((-f16::infinity).is_inf());
    CHECK(!f16::infinity.is_finite());
    CHECK(!f16::quiet_nan.is_finite());
    CHECK(f16::max.is_finite());
    CHECK(f16::denorm_min.is_subnormal());
    CHECK(!f16::min_normal.is_subnormal());
    CHECK(!f16().is_subnormal());
    CHECK(f16(-0.0f).sign_bit());
    CHECK(!f16().sign_bit());
}

TEST("tg f16 - a NaN is unordered")
{
    for (auto const nan : {f16::quiet_nan, f16::make_from_bits(0x7c01), f16::make_from_bits(0xffff)})
    {
        for (auto const other : {f16::quiet_nan, f16(1), f16(), f16::infinity})
        {
            CHECK(!(nan == other));
            CHECK(nan != other);
            CHECK(!(nan < other));
            CHECK(!(nan <= other));
            CHECK(!(nan > other));
            CHECK(!(nan >= other));
            CHECK((nan <=> other) == std::partial_ordering::unordered);
            CHECK((other <=> nan) == std::partial_ordering::unordered);

            // the f32 side the exhaustive test holds f16 against
            auto const fnan = nan.to_f32();
            auto const fother = other.to_f32();
            CHECK(fnan != fnan);
            CHECK(!(fnan == fother));
            CHECK(!(fnan < fother));
            CHECK((fnan <=> fother) == std::partial_ordering::unordered);
        }
    }
}

TEST("tg f16 - comparison matches f32's")
{
    auto mismatches = 0;
    auto first_a = u16(0);
    auto first_b = u16(0);
    auto const check_pair = [&](u16 a, u16 b)
    {
        auto const x = f16::make_from_bits(a);
        auto const y = f16::make_from_bits(b);
        auto const fx = x.to_f32();
        auto const fy = y.to_f32();
        auto const ok = (x == y) == (fx == fy) && (x != y) == (fx != fy) && (x < y) == (fx < fy)
                     && (x <= y) == (fx <= fy) && (x > y) == (fx > fy) && (x >= y) == (fx >= fy)
                     && (x <=> y) == (fx <=> fy);
        if (!ok && mismatches == 0)
        {
            first_a = a;
            first_b = b;
        }
        mismatches += ok ? 0 : 1;
    };

    u16 const specials[] = {0x0000, 0x8000, 0x0001, 0x8001, 0x03ff, 0x0400, 0x3c00, 0xbc00, 0x3c01,
                            0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7c01, 0x7e00, 0xfe00, 0xffff};
    for (auto i = 0; i < 65536; ++i)
        for (auto const s : specials)
        {
            check_pair(u16(i), s);
            check_pair(s, u16(i));
        }

    auto rng = nx::test_random();
    auto const count = nx::is_thorough() ? 50'000'000 : 500'000;
    for (auto i = 0; i < count; ++i)
        check_pair(random_half(rng), random_half(rng));
    CHECK(mismatches == 0).dump("first a", first_a).dump("first b", first_b);

    if (mismatches != 0) // spell out the first failing pair, one operator per CHECK
    {
        auto const x = f16::make_from_bits(first_a);
        auto const y = f16::make_from_bits(first_b);
        auto const fx = x.to_f32();
        auto const fy = y.to_f32();
        CHECK((x == y) == (fx == fy));
        CHECK((x != y) == (fx != fy));
        CHECK((x < y) == (fx < fy));
        CHECK((x <= y) == (fx <= fy));
        CHECK((x > y) == (fx > fy));
        CHECK((x >= y) == (fx >= fy));
        CHECK((x <=> y) == (fx <=> fy));
    }
}

TEST("tg f16 - rounding on the bits matches f32's, for every value")
{
    auto mismatches = 0;
    for (auto i = 0; i < 65536; ++i)
    {
        auto const h = f16::make_from_bits(u16(i));
        auto const x = h.to_f32();
        auto const floor = tg::floor(h);
        auto const ceil = tg::ceil(h);
        auto const round = tg::round(h);
        auto const ok = same_result(floor, f16(tg::floor(x)).bits()) && same_result(ceil, f16(tg::ceil(x)).bits())
                     && same_result(round, f16(tg::round(x)).bits());
        mismatches += ok ? 0 : 1;
        if (h.is_nan())
            CHECK((floor.bits() & 0x0200) != 0); // quiet
    }
    CHECK(mismatches == 0);
}

TEST("tg f16 - base two on the bits matches f32's")
{
    SECTION("scale_by_pow2, for every value and every shift that matters")
    {
        auto mismatches = 0;
        for (auto i = 0; i < 65536; ++i)
        {
            auto const h = f16::make_from_bits(u16(i));
            for (auto n = -45; n <= 45; ++n)
                mismatches += same_result(tg::scale_by_pow2(h, n), f16(tg::scale_by_pow2(h.to_f32(), n)).bits()) ? 0 : 1;
            for (auto const n : {-100000, 100000})
                mismatches += same_result(tg::scale_by_pow2(h, n), f16(tg::scale_by_pow2(h.to_f32(), n)).bits()) ? 0 : 1;
        }
        CHECK(mismatches == 0);
    }

    SECTION("split_pow2, for every finite non-zero value")
    {
        auto mismatches = 0;
        for (auto i = 0; i < 65536; ++i)
        {
            auto const h = f16::make_from_bits(u16(i));
            if (!h.is_finite() || tg::traits::is_zero(h))
                continue;
            auto const split = tg::split_pow2(h);
            auto const wide = tg::split_pow2(h.to_f32());
            auto const ok = split.significand.to_f32() == wide.significand && split.exponent == wide.exponent
                         && tg::exponent_of(h) == wide.exponent;
            mismatches += ok ? 0 : 1;
        }
        CHECK(mismatches == 0);
    }
}

TEST("tg f16 - arithmetic is correctly rounded binary16")
{
    auto mismatches = 0;
    auto const check_op = [&mismatches](char op, f16 x, f16 y)
    {
        auto const a = x.to_f64();
        auto const b = y.to_f64();
        auto r = f16();
        auto exact = 0.0;
        switch (op)
        {
        case '+':
            r = x + y;
            exact = a + b;
            break;
        case '-':
            r = x - y;
            exact = a - b;
            break;
        case '*':
            r = x * y;
            exact = a * b;
            break;
        default:
            r = x / y;
            exact = a / b;
            break;
        }
        // exact in f64 for + - *, and rounded once for /, which is innocuous at 53 bits against 11
        mismatches += same_result(r, expected_from(exact)) ? 0 : 1;
    };

    auto rng = nx::test_random();
    auto const count = nx::is_thorough() ? 10'000'000 : 200'000;
    for (auto i = 0; i < count; ++i)
    {
        auto const x = f16::make_from_bits(random_half(rng));
        auto const y = f16::make_from_bits(random_half(rng));
        for (auto const op : {'+', '-', '*', '/'})
            check_op(op, x, y);
    }
    CHECK(mismatches == 0);

    SECTION("sqrt, for every non-negative value")
    {
        auto sqrt_mismatches = 0;
        for (auto i = 0; i <= 0x7fff; ++i)
        {
            auto const h = f16::make_from_bits(u16(i));
            sqrt_mismatches += same_result(tg::sqrt(h), expected_from(tg::sqrt(h.to_f64()))) ? 0 : 1;
        }
        CHECK(sqrt_mismatches == 0);
    }

    SECTION("compound assignment and negation")
    {
        auto h = f16(1.5f);
        h += f16(1);
        h *= f16(2);
        h -= f16(1);
        h /= f16(4);
        CHECK(h == f16(1.0f));
        CHECK((-h).bits() == 0xbc00);
        CHECK((-f16()).bits() == 0x8000);
        CHECK(tg::abs(f16(-2.5f)) == f16(2.5f));
    }
}

TEST("tg f16 - a full scalar in tg's generic types")
{
    auto const v = tg::vec<3, f16>(f16(3), f16(0), f16(4));
    CHECK(v.length_sqr() == f16(25));
    CHECK(v.length() == f16(5));
    CHECK((v * f16(2))[2] == f16(8));
    auto const with_negative_zero = tg::vec<3, f16>(f16(3), f16(-0.0f), f16(4));
    CHECK(v == with_negative_zero);

    // pi / 180 rounds to f16 before the multiply, so 180 degrees lands one step below pi, as it would on a GPU
    auto const a = tg::angle<f16>::make_from_degree(f16(180));
    CHECK(a.radians() == f16(180) * (tg::pi<f16> / f16(180)));
    CHECK(tg::pi<f16>.bits() == 0x4248);
    CHECK(tgtest::approx(tg::cos(a).to_f32(), -1.0f));
    CHECK(tg::one<f16>() == f16(1));
}

TEST("tg f16 - hashing folds the two zeros")
{
    CHECK(cc::make_hash(f16(0.0f)) == cc::make_hash(f16(-0.0f)));
    CHECK(cc::make_hash(f16(1.0f)) != cc::make_hash(f16(2.0f)));
}

TEST("tg f16 - printing")
{
    using namespace tg::literals;

    CHECK(cc::format("{}", f16(0.1f)) == "0.1");
    CHECK(cc::format("{}", 0.3_f16) == "0.3");
    CHECK(cc::format("{}", f16(1.0f / 3.0f)) == "0.3333");
    CHECK(cc::format("{}", f16(-2)) == "-2");
    CHECK(cc::format("{}", f16::max) == "65500"); // the shortest digits that read back as the largest half
    CHECK(cc::format("{:.0f}", f16::max) == "65504");
    CHECK(cc::format("{:.5f}", f16(0.1f)) == "0.09998");
    CHECK(cc::format("{:>6}", f16(0.1f)) == "   0.1");
    CHECK(cc::format("{}", f16::infinity) == cc::format("{}", tg::scale_by_pow2(1.0f, 1000)));

    // every finite value prints as digits that read back as itself
    auto mismatches = 0;
    for (auto i = 0; i < 65536; ++i)
    {
        auto const h = f16::make_from_bits(u16(i));
        if (!h.is_finite())
            continue;
        auto parsed = 0.0;
        auto const ok = cc::from_string(cc::format("{}", h), parsed) && f16(parsed).bits() == h.bits();
        mismatches += ok ? 0 : 1;
    }
    CHECK(mismatches == 0);
}
