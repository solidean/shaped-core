#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/fixed_int/fixed_arith.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;
using tg::fi128;
using tg::fi192;
using tg::fi256;
using tg::fi32;
using tg::fi64;
using tg::fu128;
using tg::fu192;
using tg::fu256;
using tg::fu32;
using tg::fu64;

// =========================================================================================================
// The conversion and operator rules are the API, so they are pinned at compile time.
// =========================================================================================================

namespace
{
template <class A, class B>
concept addable = requires(A a, B b) { a + b; };
template <class A, class B>
concept comparable = requires(A a, B b) { a < b; };
} // namespace

// builtins convert implicitly exactly when every value fits
static_assert(std::is_convertible_v<int, fi128>);
static_assert(std::is_convertible_v<i64, fi64>);
static_assert(std::is_convertible_v<u64, fi128>);
static_assert(!std::is_convertible_v<u64, fi64>);
static_assert(!std::is_convertible_v<i64, fi32>);
static_assert(!std::is_convertible_v<int, fu128>);
static_assert(std::is_convertible_v<unsigned, fu64>);
static_assert(std::is_constructible_v<fi32, i64>); // explicitly, wrapping
static_assert(!std::is_constructible_v<fi64, bool>);
static_assert(!std::is_constructible_v<fi64, char>);

// every width change is written out
static_assert(!std::is_convertible_v<fi128, fi192>); // so `fi192 r = a * b` over fi128 does not compile
static_assert(std::is_constructible_v<fi192, fi128>);
static_assert(!std::is_constructible_v<fi128, fi192>); // narrowing is truncated_to<T>()
static_assert(std::is_constructible_v<fi128, fu64>);   // a narrower unsigned value widens losslessly
static_assert(!std::is_constructible_v<fu128, fi64>);
static_assert(!std::is_convertible_v<fi128, fu128>);
static_assert(std::is_constructible_v<fu128, fi128>); // same width, other signedness: an explicit reinterpretation

// operators take one type on both sides
static_assert(addable<fi128, fi128>);
static_assert(addable<fi128, int>);
static_assert(!addable<fi128, fi192>);
static_assert(!addable<fi128, fu128>);
static_assert(!comparable<fi128, fi192>);

// the clean-core pairs
static_assert(std::is_convertible_v<cc::i128, fi128>);
static_assert(std::is_convertible_v<cc::u128, fu128>);
static_assert(fi128(cc::imul128(-3, 4)) == fi128(-12));

// layout
static_assert(sizeof(fi32) == 4 && sizeof(fi64) == 8 && sizeof(fi192) == 24 && sizeof(fu256) == 32);
static_assert(fi32::limb_count == 1 && fi192::limb_count == 3);

// every operation is usable in a constant expression
static_assert(fi128(3) * fi128(-4) == fi128(-12));
static_assert(fi256(-7) / fi256(2) == fi256(-3));
static_assert(fi256(-7) % fi256(2) == fi256(-1));
static_assert(((fi192(1) << 130) >> 129) == fi192(2));
static_assert(fi128::min() < fi128::max() && fu128::max() > fu128(0));
static_assert(tg::mul<fi192>(fi128(1) << 80, fi128(1) << 90) == fi192(1) << 170);
static_assert(fi128(-5).to_f64() == -5.0);
static_assert(fi128(-5.75) == fi128(-5));

// =========================================================================================================
// Helpers: seeded values that hit the edges — 0, ±1, min, max, and every limb boundary.
// =========================================================================================================

namespace
{
template <class T>
T random_value(cc::random& rng)
{
    T r;
    for (auto i = 0; i < T::limb_count; ++i)
        r.limbs[i] = typename T::limb_type(rng.next_u64());
    // Thin some values down so small magnitudes and long runs of sign bits are common too.
    auto const k = int(rng.next_u64() % 4);
    if (k == 1)
        r = r >> int(rng.next_u64() % T::bits);
    else if (k == 2)
        r = r & (T::max() >> int(rng.next_u64() % T::bits));
    return r;
}

template <class T>
cc::vector<T> edge_values()
{
    auto v = cc::vector<T>();
    v.push_back(T());
    v.push_back(T(1));
    v.push_back(T::max());
    v.push_back(T::min());
    if constexpr (T::is_signed)
        v.push_back(T(-1));
    for (auto b = 31; b < T::bits; b += 32)
    {
        auto const p = T(1) << b;
        v.push_back(p);
        v.push_back(p - T(1));
        if constexpr (T::is_signed)
            v.push_back(-p);
    }
    return v;
}

/// The edge values plus some random ones; a default run takes `random_count`, a thorough one ten times that.
template <class T>
cc::vector<T> test_values(int random_count = 24)
{
    auto rng = nx::test_random();
    auto v = edge_values<T>();
    auto const n = nx::is_thorough() ? 10 * random_count : random_count;
    for (auto i = 0; i < n; ++i)
        v.push_back(random_value<T>(rng));
    return v;
}

template <class T>
void check_ring_identities()
{
    auto const vs = test_values<T>();
    for (auto const& a : vs)
        for (auto const& b : vs)
        {
            CHECK((a + b) - b == a);
            CHECK(a + b == b + a);
            CHECK(a * b == b * a);
            CHECK(a * (b + T(1)) == a * b + a);
            CHECK(-(a - b) == b - a);
            CHECK(((a < b) + (b < a) + (a == b)) == 1);
            CHECK((a ^ b ^ b) == a);
        }
}

template <class T>
void check_division_identities()
{
    auto const vs = test_values<T>();
    for (auto const& a : vs)
        for (auto const& b : vs)
        {
            if (b == T())
                continue;
            if constexpr (T::is_signed)
                if (a == T::min() && b == T(-1))
                {
                    CHECK(a / b == T::min());
                    CHECK(a % b == T());
                    continue;
                }
            auto const t = tg::div_mod_trunc(a, b);
            CHECK(t.quotient * b + t.remainder == a);
            CHECK(tg::impl::magnitude(t.remainder) < tg::impl::magnitude(b));
            CHECK((t.remainder == T() || t.remainder.is_negative() == a.is_negative()));

            auto const f = tg::div_mod_floor(a, b);
            CHECK(f.quotient * b + f.remainder == a);
            CHECK((f.remainder == T() || f.remainder.is_negative() == b.is_negative()));

            auto const c = tg::div_ceil(a, b);
            CHECK((c == f.quotient + T(f.remainder != T() ? 1 : 0)));
        }
}

template <class T>
void check_shift_identities()
{
    auto const vs = test_values<T>();
    for (auto const& a : vs)
        for (auto n = 0; n < T::bits; n += 7)
        {
            // multiplying by 2^n is shifting left, and a right shift is a floor division by 2^n
            CHECK((a << n) == a * (T(1) << n));
            if (n < T::bits - 1)
                CHECK((a >> n) == tg::div_floor(a, T(1) << n));
        }
}
} // namespace

// =========================================================================================================
// Same-width arithmetic
// =========================================================================================================

TEST("tg fixed_int - agrees with the builtins at 32 and 64 bits")
{
    auto rng = nx::test_random();
    for (auto i = 0; i < 2000; ++i)
    {
        auto const x = i64(rng.next_u64() >> (rng.next_u64() % 64));
        auto const y = i64(rng.next_u64() >> (rng.next_u64() % 64)) | 1;
        auto const n = int(rng.next_u64() % 64);

        auto const a = fi64(x);
        auto const b = fi64(y);
        CHECK((a + b).limbs[0] == u64(x) + u64(y));
        CHECK((a - b).limbs[0] == u64(x) - u64(y));
        CHECK((a * b).limbs[0] == u64(x) * u64(y));
        CHECK((a < b) == (x < y));
        CHECK((a >> n).limbs[0] == u64(x >> n));
        CHECK((a << n).limbs[0] == u64(x) << n);
        if (!(x == i64(u64(1) << 63) && y == -1))
        {
            CHECK((a / b).limbs[0] == u64(x / y));
            CHECK((a % b).limbs[0] == u64(x % y));
        }

        auto const ua = fu64(u64(x));
        auto const ub = fu64(u64(y));
        CHECK((ua / ub).limbs[0] == u64(x) / u64(y));
        CHECK((ua < ub) == (u64(x) < u64(y)));
        CHECK((ua >> n).limbs[0] == u64(x) >> n);

        auto const s = fi32(i32(x));
        auto const t = fi32(i32(y));
        CHECK(i32((s * t).limbs[0]) == i32(u32(i32(x)) * u32(i32(y))));
        CHECK(i32((s >> (n % 32)).limbs[0]) == i32(x) >> (n % 32));
        CHECK((s < t) == (i32(x) < i32(y)));
    }
}

TEST("tg fixed_int - ring identities")
{
    SECTION("fi32")
    {
        check_ring_identities<fi32>();
    }
    SECTION("fi64")
    {
        check_ring_identities<fi64>();
    }
    SECTION("fi128")
    {
        check_ring_identities<fi128>();
    }
    SECTION("fi192")
    {
        check_ring_identities<fi192>();
    }
    SECTION("fi256")
    {
        check_ring_identities<fi256>();
    }
    SECTION("fu128")
    {
        check_ring_identities<fu128>();
    }
    SECTION("fu256")
    {
        check_ring_identities<fu256>();
    }
    SECTION("fixed_int<320>")
    {
        check_ring_identities<tg::fixed_int<320>>();
    }
}

TEST("tg fixed_int - division identities")
{
    SECTION("fi32")
    {
        check_division_identities<fi32>();
    }
    SECTION("fi64")
    {
        check_division_identities<fi64>();
    }
    SECTION("fi128")
    {
        check_division_identities<fi128>();
    }
    SECTION("fi192")
    {
        check_division_identities<fi192>();
    }
    SECTION("fi256")
    {
        check_division_identities<fi256>();
    }
    SECTION("fu64")
    {
        check_division_identities<fu64>();
    }
    SECTION("fu192")
    {
        check_division_identities<fu192>();
    }
}

TEST("tg fixed_int - shift identities")
{
    SECTION("fi32")
    {
        check_shift_identities<fi32>();
    }
    SECTION("fi128")
    {
        check_shift_identities<fi128>();
    }
    SECTION("fi192")
    {
        check_shift_identities<fi192>();
    }
    SECTION("fu256")
    {
        check_shift_identities<fu256>();
    }
}

TEST("tg fixed_int - sign")
{
    static_assert(fi256(-3).sign() == -1 && fi256(0).sign() == 0 && fi256(3).sign() == 1);
    CHECK(fi32::min().sign() == -1);
    CHECK(fi32(7).sign() == 1);
    CHECK((fi192(1) << 150).sign() == 1);
    CHECK((fi192(-1) << 150).sign() == -1);
    CHECK(fi192::min().sign() == -1);
    CHECK(fu256::max().sign() == 1);
    CHECK(fu128(0).sign() == 0);
    // a determinant's sign, through the width it was computed in
    auto const det = tg::mul<fi192>(fi128(3) << 80, fi128(-5) << 90) - tg::mul<fi192>(fi128(-1), fi128(1));
    CHECK(det.sign() == -1);
}

TEST("tg fixed_int - wraparound")
{
    CHECK(-fi128::min() == fi128::min());
    CHECK(fi128::max() + fi128(1) == fi128::min());
    CHECK(fu192::max() + fu192(1) == fu192());
    CHECK(fi256::min() / fi256(-1) == fi256::min());
    CHECK(fi32::min() / fi32(-1) == fi32::min());
    CHECK((fi192(1) << 191) == fi192::min());
    CHECK((fi192::min() >> 191) == fi192(-1));
    CHECK((fu192(fi192::min()) >> 191) == fu192(1));
}

// =========================================================================================================
// Across widths
// =========================================================================================================

TEST("tg fixed_int - width changes")
{
    auto const a = fi128(-3);
    CHECK(fi256(a) == fi256(-3));
    CHECK(a.widened<fi192>() == fi192(-3));
    CHECK(fi256::max().truncated_to<fi64>() == fi64(-1));
    CHECK((fi128(1) << 100).truncated_to<fi64>() == fi64(0));
    CHECK(fi64(-1).shifted_left<fi128>(64) == -(fi128(1) << 64));
    CHECK(fu128(fi128(-1)) == fu128::max());
    CHECK(fi128(fu64(~u64(0))) == fi128(~u64(0)));
}

TEST("tg fixed_int - checked heterogeneous arithmetic")
{
    auto const p80 = fi128(1) << 80;
    auto const p90 = fi128(1) << 90;
    CHECK(tg::checked_mul<fi192>(p80, p90).has_value());
    CHECK(tg::checked_mul<fi192>(p80, p90).value() == fi192(1) << 170);
    CHECK(!tg::checked_mul<fi128>(p80, p90).has_value());
    CHECK(tg::checked_mul<fi256>(fi128::min(), fi128::min()).value() == fi256(1) << 254);

    CHECK(tg::checked_add<fi128>(fi128::max(), fi128(0)).has_value());
    CHECK(!tg::checked_add<fi128>(fi128::max(), fi128(1)).has_value());
    CHECK(tg::checked_add<fi192>(fi128::max(), fi128(1)).value() == fi192(1) << 127);
    CHECK(tg::checked_sub<fi64>(fi32::min(), fi32(1)).value() == fi64(i64(i32(0x80000000u)) - 1));
    CHECK(!tg::checked_sub<fu64>(fu64(1), fu64(2)).has_value());

    // a result narrower than an operand is allowed, and is a claim like any other
    CHECK(tg::checked_mul<fi64>(fi128(3), fi128(-4)).value() == fi64(-12));
    CHECK(!tg::checked_mul<fi64>(p80, fi128(1)).has_value());

    // unchecked, the same calls wrap modulo 2^R
    CHECK(tg::mul<fi192>(p80, p90) == fi192(1) << 170);
    CHECK(tg::add<fi64>(fi32::max(), fi32(1)) == fi64(i64(1) << 31));
}

TEST("tg fixed_int - bounded quotient")
{
    auto rng = nx::test_random();
    auto const n = nx::is_thorough() ? 20000 : 3000;
    for (auto i = 0; i < n; ++i)
    {
        // w up to 2^190, q up to 2^26, x as q * w plus a remainder weighted toward 0 and ±1
        auto w = random_value<fi192>(rng) >> int(1 + rng.next_u64() % 190);
        if (w == fi192())
            w = fi192(1);
        if (rng.next_u64() % 2 == 0)
            w = -w;
        auto const q = fi256(i64(rng.next_u64() % (u64(1) << 27)) - (i64(1) << 26));
        auto const kind = rng.next_u64() % 4;
        auto x = tg::mul<fi256>(q, w);
        if (kind == 1)
            x += fi256(1);
        else if (kind == 2)
            x -= fi256(1);
        else if (kind == 3)
            x += tg::mul<fi256>(random_value<fi64>(rng) >> 1, fi192(1)) % fi256(w);

        auto const expected_floor = tg::div_floor(x, fi256(w));
        auto const expected_ceil = tg::div_ceil(x, fi256(w));
        auto const got = tg::div_floor_ceil<fi32>(x, w);
        CHECK(fi256(got.floor) == expected_floor);
        CHECK(fi256(got.ceil) == expected_ceil);
    }

    // an x narrower than the estimate's shift: the shifted value is 0, and the correction still lands on the floor
    auto const big = fi192(1) << 150;
    CHECK(tg::div_floor_ceil<fi32>(fi64(5), big).floor == fi32(0));
    CHECK(tg::div_floor_ceil<fi32>(fi64(5), big).ceil == fi32(1));
    CHECK(tg::div_floor_ceil<fi32>(fi64(-5), big).floor == fi32(-1));
    CHECK(tg::div_floor_ceil<fi32>(fi64(-5), big).ceil == fi32(0));
    CHECK(tg::div_floor_ceil<fi32>(fi64(0), -big).floor == fi32(0));
    CHECK(tg::div_floor_ceil<fu32>(fu256(7) << 150, fu192(1) << 149).floor == fu32(14));
    CHECK(tg::div_floor_ceil<fu32>((fu256(7) << 150) + fu256(1), fu192(1) << 149).ceil == fu32(15));

    // the wide path, for a quotient type no estimate resolves
    CHECK(tg::div_floor<fi128>(fi256(-7) << 100, fi192(2)) == fi128(-7) << 99);
    CHECK(tg::div_ceil<fi128>(fi256(-7), fi192(2)) == fi128(-3));
}

#if TG_CHECK_WIDE_ARITH
TEST("tg fixed_int - a false claim fails where it is made")
{
    auto const p80 = fi128(1) << 80;
    auto const p90 = fi128(1) << 90;

    SECTION("result widths")
    {
        CHECK_ASSERTS(tg::mul<fi128>(p80, p90));
        CHECK_ASSERTS(tg::add<fi128>(fi128::max(), fi128(1)));
        CHECK_ASSERTS(tg::sub<fu64>(fu64(1), fu64(2)));
        CHECK(tg::mul<fi192>(p80, p90) == fi192(1) << 170);
    }
    SECTION("shift amounts")
    {
        CHECK_ASSERTS(fi128(1) << 128);
        CHECK_ASSERTS(fi128(1) >> -1);
        CHECK_ASSERTS(fi64(1).shifted_left<fi128>(128));
        CHECK((fi128(1) << 127) == fi128::min());
    }
    SECTION("floats in")
    {
        CHECK_ASSERTS(fi64(0x1p63));
        CHECK_ASSERTS(fi64(cc::bit_cast<f64>(u64(0x7ff) << 52)));
        CHECK(fi64(-0x1p63) == fi64::min());
    }
    SECTION("quotients")
    {
        // past 64 bits, the estimate's 128 ÷ 64 division would trap before any width check
        CHECK_ASSERTS(tg::div_floor<fi32>(fi128(1) << 100, fi64(1)));
        // past xs's two low words, the estimate drops the rest and one correction cannot recover it
        CHECK_ASSERTS(tg::div_floor<fi32>((fi256(1) << 200) + fi256(5), fi64(1)));
        // estimable, but wider than the claim
        CHECK_ASSERTS(tg::div_floor<fi32>(fi128(1) << 40, fi64(1)));
        CHECK_ASSERTS(tg::div_ceil<fi64>(fi256(1) << 100, fi192(1)));
        CHECK(tg::div_floor<fi32>(fi128(-7), fi64(2)) == fi32(-4));
    }
}
#endif

// =========================================================================================================
// Conversions, bits, printing
// =========================================================================================================

TEST("tg fixed_int - float conversions")
{
    SECTION("exact below 2^53")
    {
        CHECK(fi256(-123456789).to_f64() == -123456789.0);
        CHECK(fu128(u64(1) << 52).to_f64() == 4503599627370496.0);
        CHECK((fi192(1) << 150).to_f64() == 0x1p150);
        CHECK(fi64::min().to_f64() == -0x1p63);
    }
    SECTION("rounds to nearest, ties to even")
    {
        // 2^64 + 1 is between 2^64 and its successor, much nearer 2^64
        CHECK(((fu128(1) << 64) + fu128(1)).to_f64() == 0x1p64);
        // (2^53 + 1) * 2^100 is exactly half-way, and ties go to the even significand 2^53
        CHECK((((fu256(1) << 53) + fu256(1)) << 100).to_f64() == 0x1p153);
        // one bit far below the half-way point breaks the tie upward: the sticky bit
        CHECK(((((fu256(1) << 53) + fu256(1)) << 100) + fu256(1)).to_f64() == 0x1p153 + 0x1p101);
        CHECK(fu256::max().to_f64() == 0x1p256);
        CHECK(fu256::max().to_f32() == cc::bit_cast<f32>(u32(0x7f800000)));
        CHECK(((fu128(1) << 100) + fu128(1)).to_f32() == 0x1p100f);
    }
    SECTION("from f64 truncates toward zero")
    {
        CHECK(fi128(-2.5) == fi128(-2));
        CHECK(fi128(0x1p100) == fi128(1) << 100);
        CHECK(fi192(-0x1p191) == fi192::min());
        CHECK(fu64(0.999) == fu64(0));
        CHECK(fi32(-0x1p31) == fi32::min());
        CHECK(fi128(f32(-3.75f)) == fi128(-3));
    }
}

TEST("tg fixed_int - bit counts")
{
    CHECK(fu256(0).count_leading_zeroes() == 256);
    CHECK(fu256(1).count_leading_zeroes() == 255);
    CHECK((fu192(1) << 130).count_trailing_zeroes() == 130);
    CHECK(fu128::max().popcount() == 128);
    CHECK(fu128::max().count_leading_ones() == 128);
    CHECK(fu128(7).count_trailing_ones() == 3);
    CHECK((fu192(1) << 100).bit_width() == 101);
    CHECK((fu192(1) << 100).has_single_bit());
    CHECK(!fu192(3).has_single_bit());
    CHECK(fu32(1).count_leading_zeroes() == 31);
    CHECK(fi128(-1).magnitude_bit_width() == 1);
    CHECK(fi128::min().magnitude_bit_width() == 128);
    CHECK(fi128(0).magnitude_bit_width() == 0);
}

TEST("tg fixed_int - printing")
{
    CHECK((fu128(1) << 64).to_string() == "18446744073709551616");
    CHECK(fi256(-1).to_string() == "-1");
    CHECK(fi128::min().to_string() == "-170141183460469231731687303715884105728");
    CHECK(fu256::max().to_string() == "115792089237316195423570985008687907853269984665640564039457584007913129639935");
    CHECK(fi32(-42).to_string() == "-42");
    CHECK(fi64(0).to_string() == "0");
    CHECK(cc::format("{:x}", fi128(-255)) == "-ff");
    CHECK(cc::format("{:#X}", fu192(1) << 128) == "0X100000000000000000000000000000000");
    CHECK(cc::format("{:'}", fi128(1234567)) == "1'234'567");
    CHECK(cc::format("{:o}", fu64(8)) == "10");
    CHECK(cc::format("{:b}", fi32(5)) == "101");
    CHECK(cc::format("{:>6}", fi192(-7)) == "    -7");
    // a decimal chunk boundary: 10^19 needs the zero padding of the lower chunk
    CHECK(cc::format("{}", fu128(u64(10'000'000'000'000'000'000ull))) == "10000000000000000000");
}

TEST("tg fixed_int - scalar traits")
{
    static_assert(tg::traits::has_abs<fi128>);
    CHECK(tg::traits::is_zero(fi128()));
    CHECK(tg::traits::is_one(tg::one<fi192>()));
    CHECK(tg::scalar_traits<fi128>::abs(fi128(-4)) == fi128(4));
}

// =========================================================================================================
// clang's _BitInt is an independent implementation of every width, so it is the oracle wherever it exists.
// Targets whose ABI caps _BitInt at 128 bits (ARM, wasm) have no 512-bit one to hold every result.
// =========================================================================================================

#if defined(CC_COMPILER_CLANG) && defined(__BITINT_MAXWIDTH__) && __BITINT_MAXWIDTH__ >= 512
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbit-int-extension"

namespace
{
template <int N, bool S>
using bitint = std::conditional_t<S, _BitInt(N), unsigned _BitInt(N)>;

template <int Bits, bool S>
bitint<512, S> to_bitint(tg::impl::fixed_integer<Bits, S> const& x)
{
    // sign-extend through the top word, then assemble
    bitint<512, S> r = 0;
    for (auto i = tg::impl::word_count<Bits> - 1; i >= 0; --i)
        r = (r << 64) | bitint<512, S>(tg::impl::limb(x, i));
    if constexpr (S)
        if (x.is_negative() && Bits < 512)
            r -= bitint<512, S>(1) << (tg::impl::word_count<Bits> * 64); // the words were read as unsigned
    return r;
}

template <int Bits, bool S>
tg::impl::fixed_integer<Bits, S> from_bitint(bitint<512, S> v)
{
    tg::impl::fixed_integer<Bits, S> r;
    for (auto i = 0; i < tg::impl::word_count<Bits>; ++i)
    {
        tg::impl::set_limb(r, i, u64(v));
        v >>= 64;
    }
    return r;
}

template <int R, int A, int B, bool S>
void check_triple_against_bitint(int& checks)
{
    using fa = tg::impl::fixed_integer<A, S>;
    using fb = tg::impl::fixed_integer<B, S>;
    // 250 triples and three operations each: the pairs are what the budget goes to
    auto const as = test_values<fa>(4);
    auto const bs = test_values<fb>(4);
    for (auto const& a : as)
        for (auto const& b : bs)
        {
            auto const x = to_bitint(a);
            auto const y = to_bitint(b);
            CHECK((tg::impl::add_op<R, A, B, S>::apply(a, b) == from_bitint<R, S>(x + y)));
            CHECK((tg::impl::sub_op<R, A, B, S>::apply(a, b) == from_bitint<R, S>(x - y)));
            CHECK((tg::impl::mul_op<R, A, B, S>::apply(a, b) == from_bitint<R, S>(x * y)));
            ++checks;
        }
}

template <int R, int A, bool S>
void check_triples_b(int& checks)
{
    check_triple_against_bitint<R, A, 32, S>(checks);
    check_triple_against_bitint<R, A, 64, S>(checks);
    check_triple_against_bitint<R, A, 128, S>(checks);
    check_triple_against_bitint<R, A, 192, S>(checks);
    check_triple_against_bitint<R, A, 256, S>(checks);
}

template <int R, bool S>
void check_triples_a(int& checks)
{
    check_triples_b<R, 32, S>(checks);
    check_triples_b<R, 64, S>(checks);
    check_triples_b<R, 128, S>(checks);
    check_triples_b<R, 192, S>(checks);
    check_triples_b<R, 256, S>(checks);
}

template <int Bits, bool S>
void check_same_width_against_bitint()
{
    using T = tg::impl::fixed_integer<Bits, S>;
    auto const vs = test_values<T>();
    for (auto const& a : vs)
    {
        auto const x = bitint<Bits, S>(to_bitint(a));
        // At exactly 128 bits clang lowers _BitInt division and float conversion to compiler-rt calls
        // (__divti3, __floattidf), which clang-cl does not link; every other width is expanded inline.
        constexpr bool has_runtime_ops = Bits != 128;
        if constexpr (has_runtime_ops)
            CHECK(a.to_f64() == f64(x));
        for (auto n = 0; n < Bits; n += 5)
        {
            // shifted as unsigned: a signed _BitInt left shift that overflows is not defined to wrap
            auto const shifted = bitint<Bits, S>(bitint<Bits, false>(x) << n);
            CHECK(((a << n) == from_bitint<Bits, S>(bitint<512, S>(shifted))));
            CHECK(((a >> n) == from_bitint<Bits, S>(bitint<512, S>(x >> n))));
        }
        for (auto const& b : vs)
        {
            auto const y = bitint<Bits, S>(to_bitint(b));
            CHECK((a < b) == (x < y));
            if (has_runtime_ops && y != 0 && !(S && a == T::min() && b == T(-1)))
            {
                CHECK((a / b == from_bitint<Bits, S>(bitint<512, S>(x / y))));
                CHECK((a % b == from_bitint<Bits, S>(bitint<512, S>(x % y))));
            }
        }
    }
}
} // namespace

TEST("tg fixed_int - every triple against _BitInt")
{
    auto checks = 0;
    SECTION("signed")
    {
        check_triples_a<32, true>(checks);
        check_triples_a<64, true>(checks);
        check_triples_a<128, true>(checks);
        check_triples_a<192, true>(checks);
        check_triples_a<256, true>(checks);
        CHECK(checks > 0);
    }
    SECTION("unsigned")
    {
        check_triples_a<32, false>(checks);
        check_triples_a<64, false>(checks);
        check_triples_a<128, false>(checks);
        check_triples_a<192, false>(checks);
        check_triples_a<256, false>(checks);
        CHECK(checks > 0);
    }
}

TEST("tg fixed_int - same-width operations against _BitInt")
{
    SECTION("fi32")
    {
        check_same_width_against_bitint<32, true>();
    }
    SECTION("fi64")
    {
        check_same_width_against_bitint<64, true>();
    }
    SECTION("fi128")
    {
        check_same_width_against_bitint<128, true>();
    }
    SECTION("fi192")
    {
        check_same_width_against_bitint<192, true>();
    }
    SECTION("fi256")
    {
        check_same_width_against_bitint<256, true>();
    }
    SECTION("fu64")
    {
        check_same_width_against_bitint<64, false>();
    }
    SECTION("fu256")
    {
        check_same_width_against_bitint<256, false>();
    }
}

#pragma clang diagnostic pop
#endif
