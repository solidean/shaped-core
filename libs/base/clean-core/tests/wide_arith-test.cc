#include <clean-core/math/wide_arith.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u64 U64_MAX = 0xFFFFFFFFFFFFFFFFull;
}

// Compile-time pins: these must hold in a constant-evaluated context too.
static_assert(cc::umul128(0, 12345) == cc::u128{0, 0});
static_assert(cc::umul128(U64_MAX, U64_MAX) == cc::u128{1, 0xFFFFFFFFFFFFFFFEull});
static_assert(cc::umul128(0x100000000ull, 0x100000000ull) == cc::u128{0, 1});
static_assert(cc::imul128(-1, -1) == cc::i128{1, 0});
static_assert(cc::imul128(-1, 1) == cc::i128{U64_MAX, -1});
static_assert(cc::add_with_carry(U64_MAX, 1) == cc::carrying_add_result{0, 1});
static_assert(cc::sub_with_borrow(0, 1) == cc::borrowing_sub_result{U64_MAX, 1});

TEST("wide_arith - umul128")
{
    SECTION("zero and identity")
    {
        CHECK(cc::umul128(0, U64_MAX).lo == 0ull);
        CHECK(cc::umul128(0, U64_MAX).hi == 0ull);
        CHECK(cc::umul128(1, 0xDEADBEEFull).lo == 0xDEADBEEFull);
        CHECK(cc::umul128(1, 0xDEADBEEFull).hi == 0ull);
    }

    SECTION("full-width product")
    {
        auto const r = cc::umul128(U64_MAX, U64_MAX);
        CHECK(r.lo == 1ull);
        CHECK(r.hi == 0xFFFFFFFFFFFFFFFEull);
    }

    SECTION("exact 2^64 carry")
    {
        auto const r = cc::umul128(0x100000000ull, 0x100000000ull);
        CHECK(r.lo == 0ull);
        CHECK(r.hi == 1ull);
    }

    SECTION("low half equals the wrapping product")
    {
        u64 const xs[] = {0, 1, 2, 0xFFFF, 0x1234567890ABCDEFull, U64_MAX, 0xDEADBEEFCAFEBABEull};
        for (u64 const a : xs)
            for (u64 const b : xs)
                CHECK(cc::umul128(a, b).lo == u64(a * b));
    }
}

TEST("wide_arith - imul128")
{
    SECTION("sign handling")
    {
        CHECK(cc::imul128(-1, -1).lo == 1ull);
        CHECK(cc::imul128(-1, -1).hi == i64(0));

        CHECK(cc::imul128(-1, 1).lo == U64_MAX);
        CHECK(cc::imul128(-1, 1).hi == i64(-1));

        // -2^63 * 1 = -2^63
        auto const r = cc::imul128(i64(0x8000000000000000ull), 1);
        CHECK(r.lo == 0x8000000000000000ull);
        CHECK(r.hi == i64(-1));
    }

    SECTION("positive overflow into the high half")
    {
        auto const r = cc::imul128(i64(1) << 62, 4); // 2^64
        CHECK(r.lo == 0ull);
        CHECK(r.hi == i64(1));
    }

    SECTION("low half equals the wrapping product")
    {
        i64 const xs[] = {0, 1, -1, 2, -2, i64(0x7FFFFFFFFFFFFFFFull), i64(0x8000000000000000ull), -123456789};
        for (i64 const a : xs)
            for (i64 const b : xs)
                CHECK(cc::imul128(a, b).lo == u64(a * b));
    }
}

TEST("wide_arith - add_with_carry")
{
    CHECK(cc::add_with_carry(1, 2).value == 3ull);
    CHECK(cc::add_with_carry(1, 2).carry == 0ull);

    // overflow from the addends, from the incoming carry, and from both at once
    CHECK(cc::add_with_carry(U64_MAX, 1).value == 0ull);
    CHECK(cc::add_with_carry(U64_MAX, 1).carry == 1ull);
    CHECK(cc::add_with_carry(U64_MAX, 0, 1).value == 0ull);
    CHECK(cc::add_with_carry(U64_MAX, 0, 1).carry == 1ull);

    auto const r = cc::add_with_carry(U64_MAX, 1, 1); // 2^64 + 1
    CHECK(r.value == 1ull);
    CHECK(r.carry == 1ull);
}

TEST("wide_arith - sub_with_borrow")
{
    CHECK(cc::sub_with_borrow(5, 3).value == 2ull);
    CHECK(cc::sub_with_borrow(5, 3).borrow == 0ull);

    // borrow from the subtraction, from the incoming borrow, and from both at once
    CHECK(cc::sub_with_borrow(0, 1).value == U64_MAX);
    CHECK(cc::sub_with_borrow(0, 1).borrow == 1ull);
    CHECK(cc::sub_with_borrow(0, 0, 1).value == U64_MAX);
    CHECK(cc::sub_with_borrow(0, 0, 1).borrow == 1ull);

    auto const r = cc::sub_with_borrow(0, 1, 1); // -2
    CHECK(r.value == U64_MAX - 1);
    CHECK(r.borrow == 1ull);
}
