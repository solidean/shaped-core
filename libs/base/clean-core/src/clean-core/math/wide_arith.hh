#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/fwd.hh>

// Portable extended-precision integer primitives: the 64x64 -> 128 multiplies and
// the carry/borrow-propagating add/sub that wider arithmetic (hash mixers, bignum,
// fixed-point) is built from. All are constexpr.
//
// Backend selection (clang/gcc, incl. clang-cl, are CC_COMPILER_CLANG/GCC; only real cl.exe is MSVC):
//   * clang/gcc, every arch incl. ARM/WASM: __int128 — the whole job is a builtin 128-bit op, best codegen,
//     constexpr, and the compiler emits the native MUL/UMULH and ADC/SBB itself.
//   * MSVC cl.exe (no __int128): intrinsics. x64 has _umul128 / _mul128 / _addcarry_u64 / _subborrow_u64;
//     ARM64 has __umulh / __mulh for the multiplies but no carry intrinsic, so add/sub use the plain-u64
//     fallback. None are usable in a constant expression, so `if !consteval` routes constant evaluation to
//     the plain-u64 fallback (below).

#if defined(CC_COMPILER_MSVC)
#include <intrin.h>
#endif

namespace cc
{
/// Low and high 64-bit halves of an unsigned 128-bit value (hi is the more significant half).
struct u128
{
    u64 lo = 0;
    u64 hi = 0;

    [[nodiscard]] friend constexpr bool operator==(u128 const&, u128 const&) = default;
};

/// Low and high halves of a signed 128-bit value: lo is the raw low bit pattern, hi carries the sign.
struct i128
{
    u64 lo = 0;
    i64 hi = 0;

    [[nodiscard]] friend constexpr bool operator==(i128 const&, i128 const&) = default;
};

/// A 64-bit sum paired with its carry-out (0 or 1). See add_with_carry.
struct carrying_add_result
{
    u64 value = 0;
    u64 carry = 0;

    [[nodiscard]] friend constexpr bool operator==(carrying_add_result const&, carrying_add_result const&) = default;
};

/// A 64-bit difference paired with its borrow-out (0 or 1). See sub_with_borrow.
struct borrowing_sub_result
{
    u64 value = 0;
    u64 borrow = 0;

    [[nodiscard]] friend constexpr bool operator==(borrowing_sub_result const&, borrowing_sub_result const&) = default;
};

// Plain-u64 fallbacks. Only instantiated on MSVC (its consteval path, and the ARM64 add/sub path that has
// no carry intrinsic); the clang/gcc __int128 backend never references them.
#if defined(CC_COMPILER_MSVC)
namespace impl
{
[[nodiscard]] constexpr u128 wide_umul128(u64 a, u64 b)
{
    // 32x32 schoolbook: split each operand, sum the partial products through a carry-aware middle term.
    u64 const a_lo = a & 0xffffffffu, a_hi = a >> 32;
    u64 const b_lo = b & 0xffffffffu, b_hi = b >> 32;

    u64 const ll = a_lo * b_lo;
    u64 const lh = a_lo * b_hi;
    u64 const hl = a_hi * b_lo;
    u64 const hh = a_hi * b_hi;

    u64 const cross = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
    u64 const lo = (ll & 0xffffffffu) | (cross << 32);
    u64 const hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    return {lo, hi};
}

[[nodiscard]] constexpr i128 wide_imul128(i64 a, i64 b)
{
    // Signed product from the unsigned one: identical low half; correct the high half by
    // subtracting the other operand's raw bits for each negative operand (two's-complement fixup).
    u128 const u = wide_umul128(u64(a), u64(b));
    u64 hi = u.hi;
    if (a < 0)
        hi -= u64(b);
    if (b < 0)
        hi -= u64(a);
    return {u.lo, i64(hi)};
}

[[nodiscard]] constexpr carrying_add_result wide_add_with_carry(u64 a, u64 b, u64 carry_in)
{
    u64 const s = a + carry_in;
    u64 const c1 = (s < a) ? 1u : 0u;
    u64 const r = s + b;
    u64 const c2 = (r < s) ? 1u : 0u;
    return {r, c1 + c2};
}

[[nodiscard]] constexpr borrowing_sub_result wide_sub_with_borrow(u64 a, u64 b, u64 borrow_in)
{
    u64 const d = a - b;
    u64 const b1 = (a < b) ? 1u : 0u;
    u64 const r = d - borrow_in;
    u64 const b2 = (d < borrow_in) ? 1u : 0u;
    return {r, b1 + b2};
}
} // namespace impl
#endif

/// Full 128-bit product of two unsigned 64-bit values (never overflows).
[[nodiscard]] constexpr u128 umul128(u64 a, u64 b)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    __uint128_t const p = static_cast<__uint128_t>(a) * b;
    return {u64(p), u64(p >> 64)};
#else // CC_COMPILER_MSVC
    if !consteval
    {
#if defined(CC_ARCH_ARM64)
        // ARM64 cl.exe has no _umul128; __umulh gives the high half, the low half is the plain product.
        return {a * b, __umulh(a, b)};
#else
        u64 hi = 0;
        u64 const lo = _umul128(a, b, &hi);
        return {lo, hi};
#endif
    }
    return impl::wide_umul128(a, b);
#endif
}

/// Full 128-bit product of two signed 64-bit values (never overflows).
[[nodiscard]] constexpr i128 imul128(i64 a, i64 b)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    __int128 const p = static_cast<__int128>(a) * b;
    auto const u = static_cast<__uint128_t>(p);
    return {u64(u), i64(u >> 64)};
#else // CC_COMPILER_MSVC
    if !consteval
    {
#if defined(CC_ARCH_ARM64)
        // ARM64 cl.exe has no _mul128; __mulh gives the signed high half, the low half is the raw product.
        return {u64(a) * u64(b), __mulh(a, b)};
#else
        __int64 hi = 0;
        __int64 const lo = _mul128(a, b, &hi);
        return {u64(lo), i64(hi)};
#endif
    }
    return impl::wide_imul128(a, b);
#endif
}

/// Computes a + b + carry_in. carry_in must be 0 or 1; the returned carry is likewise 0 or 1.
[[nodiscard]] constexpr carrying_add_result add_with_carry(u64 a, u64 b, u64 carry_in = 0)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    __uint128_t const s = static_cast<__uint128_t>(a) + b + carry_in;
    return {u64(s), u64(s >> 64)};
#elif defined(CC_ARCH_ARM64) // MSVC ARM64: no _addcarry_u64 intrinsic
    return impl::wide_add_with_carry(a, b, carry_in);
#else                        // MSVC x64
    if !consteval
    {
        u64 out = 0;
        unsigned char const c = _addcarry_u64(static_cast<unsigned char>(carry_in), a, b, &out);
        return {out, c};
    }
    return impl::wide_add_with_carry(a, b, carry_in);
#endif
}

/// Computes a - b - borrow_in. borrow_in must be 0 or 1; the returned borrow is likewise 0 or 1.
[[nodiscard]] constexpr borrowing_sub_result sub_with_borrow(u64 a, u64 b, u64 borrow_in = 0)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    // On underflow the 128-bit difference wraps, leaving the high half all-ones; bit 0 of it is the borrow.
    __uint128_t const d = static_cast<__uint128_t>(a) - b - borrow_in;
    return {u64(d), static_cast<u64>(d >> 64) & 1u};
#elif defined(CC_ARCH_ARM64) // MSVC ARM64: no _subborrow_u64 intrinsic
    return impl::wide_sub_with_borrow(a, b, borrow_in);
#else                        // MSVC x64
    if !consteval
    {
        u64 out = 0;
        unsigned char const bw = _subborrow_u64(static_cast<unsigned char>(borrow_in), a, b, &out);
        return {out, bw};
    }
    return impl::wide_sub_with_borrow(a, b, borrow_in);
#endif
}
} // namespace cc
