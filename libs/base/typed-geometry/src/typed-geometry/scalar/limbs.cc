#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <typed-geometry/scalar/impl/limbs.hh>

using namespace cc::primitive_defines;

u64 tg::impl::limbs_addmul_1(u64* r, u64 const* a, i32 n, u64 b)
{
#if TG_LIMBS_X64_ASM && defined(__BMI2__)
    // each limb's carries are taken up by its own adc, so none has to survive the loop's inc
    if (n <= 0)
        return 0;
    auto i = -i64(n);
    u64 carry = 0;
    u64 lo;
    u64 hi;
    asm volatile("1:\n\t"
                 "mulx (%[a],%[i],8), %[lo], %[hi]\n\t"
                 "add %[c], %[lo]\n\t"
                 "adc $0, %[hi]\n\t"
                 "add %[lo], (%[r],%[i],8)\n\t"
                 "adc $0, %[hi]\n\t"
                 "mov %[hi], %[c]\n\t"
                 "inc %[i]\n\t"
                 "jnz 1b"
                 : [i] "+r"(i), [c] "+r"(carry), [lo] "=&r"(lo), [hi] "=&r"(hi)
                 : [a] "r"(a + n), [r] "r"(r + n), "d"(b)
                 : "cc", "memory");
    return carry;
#else
    u64 carry = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const p = limb_mul_add(r[i], a[i], b, carry);
        r[i] = p.lo;
        carry = p.hi;
    }
    return carry;
#endif
}

u64 tg::impl::limbs_submul_1(u64* r, u64 const* a, i32 n, u64 b)
{
#if TG_LIMBS_X64_ASM && defined(__BMI2__)
    // as limbs_addmul_1; the high limb cannot overflow, since a carry out of the low add leaves 0 to subtract
    if (n <= 0)
        return 0;
    auto i = -i64(n);
    u64 carry = 0;
    u64 lo;
    u64 hi;
    asm volatile("1:\n\t"
                 "mulx (%[a],%[i],8), %[lo], %[hi]\n\t"
                 "add %[c], %[lo]\n\t"
                 "adc $0, %[hi]\n\t"
                 "sub %[lo], (%[r],%[i],8)\n\t"
                 "adc $0, %[hi]\n\t"
                 "mov %[hi], %[c]\n\t"
                 "inc %[i]\n\t"
                 "jnz 1b"
                 : [i] "+r"(i), [c] "+r"(carry), [lo] "=&r"(lo), [hi] "=&r"(hi)
                 : [a] "r"(a + n), [r] "r"(r + n), "d"(b)
                 : "cc", "memory");
    return carry;
#else
    u64 carry = 0;
    u64 borrow = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const p = limb_mul_add(0, a[i], b, carry);
        carry = p.hi;
        auto const d = cc::sub_with_borrow(r[i], p.lo, borrow);
        r[i] = d.value;
        borrow = d.borrow;
    }
    return carry + borrow;
#endif
}

namespace
{
// Product scanning with both sizes known: each column of partial products summed in three registers and stored once.
// Unrolled by template recursion rather than by loops with constant bounds, which the optimizer need not unroll at 8 x 8.

#if (defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)) && defined(__x86_64__) && defined(__BMI2__)
#define TG_LIMBS_MULX_ASM 1
#else
#define TG_LIMBS_MULX_ASM 0
#endif

/// (c2, c1, c0) += x * y, y read where it lies.
/// As C++ the carry into c2 comes out of clang as setb, movzx and spills, in either the carry intrinsics or a 128-bit sum;
/// as one asm statement it is mulx, add, adc, adc.
CC_FORCE_INLINE void mul_accumulate(u64 x, u64 const& y, u64& c0, u64& c1, u64& c2)
{
#if TG_LIMBS_MULX_ASM
    u64 lo;
    u64 hi;
    // "m" rather than "rm": with the choice, clang copies y through a stack slot first
    asm("mulx %[y], %[lo], %[hi]\n\t"
        "add %[lo], %[c0]\n\t"
        "adc %[hi], %[c1]\n\t"
        "adc $0, %[c2]"
        : [lo] "=&r"(lo), [hi] "=&r"(hi), [c0] "+r"(c0), [c1] "+r"(c1), [c2] "+r"(c2)
        : "d"(x), [y] "m"(y)
        : "cc");
#else
    auto const p = cc::umul128(x, y);
    auto const s0 = cc::add_with_carry(c0, p.lo);
    auto const s1 = cc::add_with_carry(c1, p.hi, s0.carry);
    c0 = s0.value;
    c1 = s1.value;
    c2 += s1.carry;
#endif
}

/// The products a[I] * b[K - I] of column K, from I on.
template <int A, int B, int K, int I>
CC_FORCE_INLINE void mul_column(u64 const* a, u64 const* b, u64& c0, u64& c1, u64& c2)
{
    if constexpr (I < A)
    {
        constexpr int j = K - I;
        if constexpr (j >= 0 && j < B)
            mul_accumulate(a[I], b[j], c0, c1, c2);
        mul_column<A, B, K, I + 1>(a, b, c0, c1, c2);
    }
}

/// Columns K and up, each stored as it completes.
template <int A, int B, int K>
CC_FORCE_INLINE void mul_columns(u64* r, u64 const* a, u64 const* b, u64 c0, u64 c1)
{
    if constexpr (K < A + B - 1)
    {
        u64 c2 = 0;
        mul_column<A, B, K, 0>(a, b, c0, c1, c2);
        r[K] = c0;
        mul_columns<A, B, K + 1>(r, a, b, c1, c2);
    }
    else
        r[K] = c0;
}

template <int A, int B>
void mul_fixed(u64* r, u64 const* a, u64 const* b)
{
    mul_columns<A, B, 0>(r, a, b, 0, 0);
}

/// The largest operands mul_fixed covers in every shape, in limbs: 256 bits.
constexpr i32 mul_fixed_limit = 4;
/// The largest square mul_fixed covers: 1024 bits, the base Karatsuba splits 32 limbs down to.
/// Unrolled, a product is five instructions, mulx and an add chain, which beats Karatsuba's recombination below this.
constexpr i32 mul_square_limit = 16;

using mul_fixed_fn = void (*)(u64*, u64 const*, u64 const*);
constexpr mul_fixed_fn mul_fixed_table[mul_fixed_limit][mul_fixed_limit] = {
    {mul_fixed<1, 1>, mul_fixed<1, 2>, mul_fixed<1, 3>, mul_fixed<1, 4>},
    {mul_fixed<2, 1>, mul_fixed<2, 2>, mul_fixed<2, 3>, mul_fixed<2, 4>},
    {mul_fixed<3, 1>, mul_fixed<3, 2>, mul_fixed<3, 3>, mul_fixed<3, 4>},
    {mul_fixed<4, 1>, mul_fixed<4, 2>, mul_fixed<4, 3>, mul_fixed<4, 4>},
};
constexpr mul_fixed_fn mul_square_table[mul_square_limit] = {
    mul_fixed<1, 1>,   mul_fixed<2, 2>,   mul_fixed<3, 3>,   mul_fixed<4, 4>,   mul_fixed<5, 5>,   mul_fixed<6, 6>,
    mul_fixed<7, 7>,   mul_fixed<8, 8>,   mul_fixed<9, 9>,   mul_fixed<10, 10>, mul_fixed<11, 11>, mul_fixed<12, 12>,
    mul_fixed<13, 13>, mul_fixed<14, 14>, mul_fixed<15, 15>, mul_fixed<16, 16>,
};

/// r[0, an + bn) = a * b in rows, a written by the first and added into by the rest.
void mul_rows(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    r[an] = tg::impl::limbs_mul_1(r, a, an, b[0]);
    for (auto j = 1; j < bn; ++j)
        r[an + j] = tg::impl::limbs_addmul_1(r + j, a, an, b[j]);
}

/// The square size from which Karatsuba applies: past the unrolled squares, and its halves land on them.
constexpr i32 karatsuba_threshold = mul_square_limit + 1;

/// The scratch mul_karatsuba needs for n limbs: per level the two differences and their product.
constexpr i32 karatsuba_scratch(i32 n)
{
    if (n < karatsuba_threshold)
        return 0;
    auto const m = (n + 1) / 2;
    return 4 * m + karatsuba_scratch(m);
}

CC_FORCE_INLINE void mul_square_basecase(u64* r, u64 const* a, u64 const* b, i32 n)
{
    if (n <= mul_square_limit)
        mul_square_table[n - 1](r, a, b);
    else
        mul_rows(r, a, n, b, n);
}

/// Adds the small value c into r[0, n), carrying up; the caller knows the sum fits.
CC_FORCE_INLINE void add_small(u64* r, i32 n, u64 c)
{
    if (c != 0)
        (void)tg::impl::limbs_add(r, r, n, &c, 1);
}

/// d[0, xn) = |x - y| for xn >= yn, y read as zero above yn; true when y > x.
bool sub_abs(u64* d, u64 const* x, i32 xn, u64 const* y, i32 yn)
{
    auto x_larger = false;
    for (auto i = yn; i < xn && !x_larger; ++i)
        x_larger = x[i] != 0;
    if (x_larger || tg::impl::limbs_cmp(x, y, yn) >= 0)
    {
        (void)tg::impl::limbs_sub(d, x, xn, y, yn);
        return false;
    }
    (void)tg::impl::limbs_sub(d, y, yn, x, yn);
    for (auto i = yn; i < xn; ++i)
        d[i] = 0;
    return true;
}

/// r[0, 2n) = a * b for n-limb operands, in Karatsuba's subtractive form:
/// a * b = z0 + (z0 + z2 - (a0 - a1)(b0 - b1)) * 2^(64m) + z2 * 2^(128m), where every product is m x m or smaller.
/// scratch holds karatsuba_scratch(n) limbs; r overlaps nothing.
void mul_karatsuba(u64* r, u64 const* a, u64 const* b, i32 n, u64* scratch)
{
    auto const m = (n + 1) / 2;
    auto const h = n - m;
    auto const half = [](u64* out, u64 const* x, u64 const* y, i32 k, u64* below)
    {
        if (k < karatsuba_threshold)
            mul_square_basecase(out, x, y, k);
        else
            mul_karatsuba(out, x, y, k, below);
    };
    half(r, a, b, m, scratch);
    half(r + 2 * m, a + m, b + m, h, scratch);

    auto* const da = scratch;
    auto* const db = scratch + m;
    auto* const zm = scratch + 2 * m;
    auto const negative_a = sub_abs(da, a, m, a + m, h);
    auto const negative_b = sub_abs(db, b, m, b + m, h);
    half(zm, da, db, m, scratch + 4 * m);

    // r is z0 = (L0, H0) then z2 = (L2, H2), each part m limbs but H2, which is 2h - m.
    // The middle adds z0 + z2 at m: H0 gains L0 + L2, and L2 gains H0 + H2, so H = H0 + L2 serves both, as GMP's toom22 has it.
    auto* const h0 = r + m;
    auto* const l2 = r + 2 * m;
    auto* const h2 = r + 3 * m;
    auto const top = 2 * n - 3 * m;
    auto const c_h = tg::impl::limbs_add_n(l2, h0, l2, m);
    auto const c_low = tg::impl::limbs_add_n(h0, l2, r, m);
    auto const c_high = tg::impl::limbs_add(l2, l2, m, h2, top);
    add_small(l2, m + top, c_h + c_low);
    add_small(h2, top, c_h + c_high);

    // and loses (a0 - a1)(b0 - b1), which is zm with the sign of the two differences together
    if (negative_a == negative_b)
        (void)tg::impl::limbs_sub(h0, h0, 2 * n - m, zm, 2 * m);
    else
        (void)tg::impl::limbs_add(h0, h0, 2 * n - m, zm, 2 * m);
}

/// The operand size up to which Karatsuba's scratch stays on the stack.
constexpr i32 karatsuba_stack_size = 64;
/// 2 KiB: past 4 KiB of frame Windows probes the stack page by page on every call.
constexpr i32 karatsuba_stack_limbs = 256;
static_assert(karatsuba_scratch(karatsuba_stack_size) <= karatsuba_stack_limbs);

/// Karatsuba for operands of the same size, at least the threshold.
void mul_balanced(u64* r, u64 const* a, u64 const* b, i32 n)
{
    if (n <= karatsuba_stack_size)
    {
        u64 scratch[karatsuba_stack_limbs];
        mul_karatsuba(r, a, b, n, scratch);
        return;
    }
    auto scratch = cc::vector<u64>();
    scratch.resize_to_uninitialized(karatsuba_scratch(n));
    mul_karatsuba(r, a, b, n, scratch.data());
}
} // namespace

void tg::impl::limbs_mul(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    if (an < bn)
    {
        cc::swap(a, b);
        cc::swap(an, bn);
    }
    if (an <= mul_fixed_limit)
    {
        mul_fixed_table[an - 1][bn - 1](r, a, b);
        return;
    }
    if (an == bn)
    {
        if (an <= mul_square_limit)
            mul_square_table[an - 1](r, a, b);
        else if (an < karatsuba_threshold)
            mul_rows(r, a, an, b, bn);
        else
            mul_balanced(r, a, b, an);
        return;
    }
    if (bn < karatsuba_threshold)
    {
        mul_rows(r, a, an, b, bn);
        return;
    }

    // a longer than b, both large: a in chunks of b's size, each product added into r where the previous one ended
    mul_balanced(r, a, b, bn);
    auto tmp = cc::vector<u64>();
    tmp.resize_to_uninitialized(2 * bn);
    for (auto off = bn; off < an; off += bn)
    {
        auto const len = an - off < bn ? an - off : bn;
        limbs_mul(tmp.data(), b, bn, a + off, len);
        // r[off, off + bn) holds the previous product's top; above it nothing is written yet
        auto const carry = limbs_add(r + off, tmp.data(), bn, r + off, bn);
        for (auto i = bn; i < bn + len; ++i)
            r[off + i] = tmp[i];
        if (carry != 0)
            (void)limbs_add(r + off + bn, r + off + bn, len, &carry, 1);
    }
}

u64 tg::impl::limbs_divrem_1(u64* q, u64 const* a, i32 n, u64 d)
{
    u64 rem = 0;
    for (auto i = n - 1; i >= 0; --i)
    {
        auto const x = cc::udiv128({a[i], rem}, d);
        q[i] = x.quotient;
        rem = x.remainder;
    }
    return rem;
}

u64 tg::impl::limbs_lshift(u64* r, u64 const* a, i32 n, int s)
{
    CC_ASSERT(0 < s && s < 64, "a limb shift is within one limb");
    auto const out = a[n - 1] >> (64 - s);
    for (auto i = n - 1; i > 0; --i)
        r[i] = (a[i] << s) | (a[i - 1] >> (64 - s));
    r[0] = a[0] << s;
    return out;
}

u64 tg::impl::limbs_rshift(u64* r, u64 const* a, i32 n, int s)
{
    CC_ASSERT(0 < s && s < 64, "a limb shift is within one limb");
    auto const out = a[0] << (64 - s);
    for (auto i = 0; i < n - 1; ++i)
        r[i] = (a[i] >> s) | (a[i + 1] << (64 - s));
    r[n - 1] = a[n - 1] >> s;
    return out;
}

void tg::impl::limbs_divrem(u64* q, u64* r, u64 const* a, i32 an, u64 const* b, i32 bn, u64* scratch)
{
    CC_ASSERT(bn >= 2 && an >= bn && b[bn - 1] != 0, "limbs_divrem takes normalized operands, the divisor of two limbs "
                                                     "or more");
    auto* const un = scratch;
    auto* const vn = scratch + an + 1;

    // shift both so the divisor's top bit is set, which bounds each digit's estimate to two corrections
    auto const s = cc::count_leading_zeroes(b[bn - 1]);
    if (s == 0)
    {
        for (auto i = 0; i < bn; ++i)
            vn[i] = b[i];
        for (auto i = 0; i < an; ++i)
            un[i] = a[i];
        un[an] = 0;
    }
    else
    {
        limbs_lshift(vn, b, bn, s);
        un[an] = limbs_lshift(un, a, an, s);
    }

    auto const vtop = vn[bn - 1];
    auto const vnext = vn[bn - 2];
    for (auto j = an - bn; j >= 0; --j)
    {
        // estimate the digit from the top two limbs of the running remainder over the top divisor limb
        // un[j + bn] <= vtop holds, and equality is the one case udiv128 cannot take
        u64 qhat = 0;
        u64 rhat = 0;
        auto rhat_overflow = false;
        if (un[j + bn] >= vtop)
        {
            qhat = ~u64(0);
            auto const s1 = cc::add_with_carry(un[j + bn - 1], vtop);
            rhat = s1.value;
            rhat_overflow = s1.carry != 0;
        }
        else
        {
            auto const d = cc::udiv128({un[j + bn - 1], un[j + bn]}, vtop);
            qhat = d.quotient;
            rhat = d.remainder;
        }
        while (!rhat_overflow)
        {
            auto const p = cc::umul128(qhat, vnext);
            if (p.hi < rhat || (p.hi == rhat && p.lo <= un[j + bn - 2]))
                break;
            --qhat;
            auto const s1 = cc::add_with_carry(rhat, vtop);
            rhat = s1.value;
            rhat_overflow = s1.carry != 0;
        }

        // multiply and subtract; a final borrow means the estimate was one too large, so the divisor is added back
        auto const top = cc::sub_with_borrow(un[j + bn], limbs_submul_1(un + j, vn, bn, qhat));
        un[j + bn] = top.value;
        if (top.borrow != 0)
        {
            --qhat;
            un[j + bn] += limbs_add(un + j, un + j, bn, vn, bn);
        }
        q[j] = qhat;
    }

    if (s == 0)
        for (auto i = 0; i < bn; ++i)
            r[i] = un[i];
    else
        limbs_rshift(r, un, bn, s);
}
