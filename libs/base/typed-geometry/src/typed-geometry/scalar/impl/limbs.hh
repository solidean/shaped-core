#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/math/wide_arith.hh>
#include <typed-geometry/scalar/fwd.hh>

// Natural numbers as arrays of 64-bit limbs, least significant first: the kernels under tg::bigint and tg::bigfloat.
// Non-template and unchecked: a size is a limb count, and the caller sizes every output.
// An output may alias an input only where its function says so.

namespace tg::impl
{
/// acc + a * b + carry as a 128-bit value; it cannot overflow, since (2^64 - 1)^2 + 2 * (2^64 - 1) = 2^128 - 1.
[[nodiscard]] CC_FORCE_INLINE cc::u128 limb_mul_add(u64 acc, u64 a, u64 b, u64 carry)
{
#if defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)
    auto const s = static_cast<__uint128_t>(a) * b + acc + carry;
    return {u64(s), u64(s >> 64)};
#else
    auto const p = cc::umul128(a, b);
    auto const s1 = cc::add_with_carry(p.lo, acc);
    auto const s2 = cc::add_with_carry(s1.value, carry);
    return {s2.value, p.hi + s1.carry + s2.carry};
#endif
}

/// n with the zero limbs at the top of a[0, n) dropped.
[[nodiscard]] CC_FORCE_INLINE i32 limbs_normalized_size(u64 const* a, i32 n)
{
    while (n > 0 && a[n - 1] == 0)
        --n;
    return n;
}

/// The sign of a - b, both of n limbs.
[[nodiscard]] CC_FORCE_INLINE int limbs_cmp(u64 const* a, u64 const* b, i32 n)
{
    for (auto i = n - 1; i >= 0; --i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/// The sign of a - b for normalized a[0, an) and b[0, bn).
[[nodiscard]] CC_FORCE_INLINE int limbs_cmp(u64 const* a, i32 an, u64 const* b, i32 bn)
{
    if (an != bn)
        return an < bn ? -1 : 1;
    return limbs_cmp(a, b, an);
}

#if (defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)) && defined(__x86_64__)
#define TG_LIMBS_X64_ASM 1
#else
#define TG_LIMBS_X64_ASM 0
#endif

/// r[0, n) = a[0, n) + b[0, n), returning the carry out; r may be a or b.
/// On x64 one adc per limb, its index counting up to zero by inc, which leaves the carry flag alone:
/// as C++ the loop's own compare clobbers it, and each limb pays to save and restore it.
CC_FORCE_INLINE u64 limbs_add_n(u64* r, u64 const* a, u64 const* b, i32 n)
{
#if TG_LIMBS_X64_ASM
    if (n <= 0)
        return 0;
    auto i = -i64(n);
    u64 t;
    u64 carry;
    asm volatile("clc\n\t"
                 "1:\n\t"
                 "mov (%[a],%[i],8), %[t]\n\t"
                 "adc (%[b],%[i],8), %[t]\n\t"
                 "mov %[t], (%[r],%[i],8)\n\t"
                 "inc %[i]\n\t"
                 "jnz 1b\n\t"
                 "sbb %[c], %[c]"
                 : [i] "+r"(i), [t] "=&r"(t), [c] "=r"(carry)
                 : [a] "r"(a + n), [b] "r"(b + n), [r] "r"(r + n)
                 : "cc", "memory");
    return carry & 1;
#else
    u64 carry = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const s = cc::add_with_carry(a[i], b[i], carry);
        r[i] = s.value;
        carry = s.carry;
    }
    return carry;
#endif
}

/// r[0, n) = a[0, n) - b[0, n), returning the borrow out; r may be a or b.
CC_FORCE_INLINE u64 limbs_sub_n(u64* r, u64 const* a, u64 const* b, i32 n)
{
#if TG_LIMBS_X64_ASM
    if (n <= 0)
        return 0;
    auto i = -i64(n);
    u64 t;
    u64 borrow;
    asm volatile("clc\n\t"
                 "1:\n\t"
                 "mov (%[a],%[i],8), %[t]\n\t"
                 "sbb (%[b],%[i],8), %[t]\n\t"
                 "mov %[t], (%[r],%[i],8)\n\t"
                 "inc %[i]\n\t"
                 "jnz 1b\n\t"
                 "sbb %[c], %[c]"
                 : [i] "+r"(i), [t] "=&r"(t), [c] "=r"(borrow)
                 : [a] "r"(a + n), [b] "r"(b + n), [r] "r"(r + n)
                 : "cc", "memory");
    return borrow & 1;
#else
    u64 borrow = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const s = cc::sub_with_borrow(a[i], b[i], borrow);
        r[i] = s.value;
        borrow = s.borrow;
    }
    return borrow;
#endif
}

/// r[0, an) = a[0, an) + b[0, bn) with an >= bn, returning the carry out.
/// r may be a or b: limb i is read before it is written.
/// Inline, since at the sizes a predicate meets a call costs as much as the loop.
CC_FORCE_INLINE u64 limbs_add(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    u64 carry = limbs_add_n(r, a, b, bn);
    auto i = bn;
    for (; i < an && carry != 0; ++i)
    {
        auto const s = cc::add_with_carry(a[i], 0, carry);
        r[i] = s.value;
        carry = s.carry;
    }
    if (r != a)
        for (; i < an; ++i)
            r[i] = a[i];
    return carry;
}

/// r[0, an) = a[0, an) - b[0, bn) with an >= bn, returning the borrow out, which is 0 exactly when a >= b.
/// r may be a or b.
CC_FORCE_INLINE u64 limbs_sub(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    u64 borrow = limbs_sub_n(r, a, b, bn);
    auto i = bn;
    for (; i < an && borrow != 0; ++i)
    {
        auto const s = cc::sub_with_borrow(a[i], 0, borrow);
        r[i] = s.value;
        borrow = s.borrow;
    }
    if (r != a)
        for (; i < an; ++i)
            r[i] = a[i];
    return borrow;
}

/// r[0, an + bn) = a[0, an) * b[0, bn), with an, bn >= 1; r overlaps neither operand.
void limbs_mul(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn);

/// r[0, n) = a[0, n) * b, returning the limb above; r may be a.
CC_FORCE_INLINE u64 limbs_mul_1(u64* r, u64 const* a, i32 n, u64 b)
{
    u64 carry = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const p = limb_mul_add(0, a[i], b, carry);
        r[i] = p.lo;
        carry = p.hi;
    }
    return carry;
}

/// r[0, n) += a[0, n) * b, returning the limb above; r overlaps a only if it is a.
u64 limbs_addmul_1(u64* r, u64 const* a, i32 n, u64 b);

/// r[0, n) -= a[0, n) * b, returning the limb to subtract above; r overlaps a only if it is a.
u64 limbs_submul_1(u64* r, u64 const* a, i32 n, u64 b);

/// q[0, n) = a[0, n) / d, returning the remainder; d != 0, and q may be a.
u64 limbs_divrem_1(u64* q, u64 const* a, i32 n, u64 d);

/// The scratch limbs limbs_divrem needs: the normalized dividend with a limb on top, and the normalized divisor.
[[nodiscard]] constexpr i32 limbs_divrem_scratch(i32 an, i32 bn)
{
    return an + 1 + bn;
}

/// q[0, an - bn + 1) = a / b and r[0, bn) = a % b, for normalized a and b with an >= bn >= 2.
/// Knuth's Algorithm D, each digit estimated by cc::udiv128 and corrected at most twice.
/// scratch holds limbs_divrem_scratch(an, bn) limbs; q and r overlap nothing, a and b included.
void limbs_divrem(u64* q, u64* r, u64 const* a, i32 an, u64 const* b, i32 bn, u64* scratch);

/// r[0, n) = a[0, n) << s for 0 < s < 64, returning the bits shifted out at the top.
/// Runs from the top down, so r may be a or lie above it.
u64 limbs_lshift(u64* r, u64 const* a, i32 n, int s);

/// r[0, n) = a[0, n) >> s for 0 < s < 64, returning the bits shifted out at the bottom, in the high bits of the limb.
/// Runs from the bottom up, so r may be a or lie below it.
u64 limbs_rshift(u64* r, u64 const* a, i32 n, int s);
} // namespace tg::impl
