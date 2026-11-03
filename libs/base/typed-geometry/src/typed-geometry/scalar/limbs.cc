#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <typed-geometry/scalar/impl/limbs.hh>

using namespace cc::primitive_defines;

#if TG_LIMBS_X64_ASM && defined(__BMI2__)
// The pair loop of limbs_addmul_1 and limbs_submul_1, with OP add or sub and OPC adc or sbb, on an even n.
// Two limbs per round: the first product's high limb is added into the second's low off the carry's path,
// which leaves add, adc, OPC, adc on it, two cycles a limb where a limb at a time takes three.
// The high limb cannot overflow: a product's is at most 2^64 - 2, and a carry or borrow out of the pair leaves room for it.
#define TG_LIMBS_MUL_1_PAIRS_ASM(OP, OPC)                                               \
    "1:\n\t"                                                                            \
    "mulx (%[a],%[i],8), %[l0], %[h0]\n\t"                                              \
    "mulx 8(%[a],%[i],8), %[l1], %[h1]\n\t"                                             \
    "add %[h0], %[l1]\n\t"                                                              \
    "adc $0, %[h1]\n\t"                                                                 \
    "add %[c], %[l0]\n\t"                                                               \
    "adc $0, %[l1]\n\t"                                                                 \
    "adc $0, %[h1]\n\t" OP " %[l0], (%[r],%[i],8)\n\t" OPC " %[l1], 8(%[r],%[i],8)\n\t" \
    "adc $0, %[h1]\n\t"                                                                 \
    "mov %[h1], %[c]\n\t"                                                               \
    "add $2, %[i]\n\t"                                                                  \
    "jnz 1b"
#endif

u64 tg::impl::limbs_addmul_1(u64* r, u64 const* a, i32 n, u64 b)
{
#if TG_LIMBS_X64_ASM && defined(__BMI2__)
    if (n <= 0)
        return 0;
    u64 carry = 0;
    if ((n & 1) != 0)
    {
        auto const p = limb_mul_add(r[0], a[0], b, 0);
        r[0] = p.lo;
        carry = p.hi;
        ++r;
        ++a;
        --n;
        if (n == 0)
            return carry;
    }
    auto i = -i64(n);
    u64 l0;
    u64 h0;
    u64 l1;
    u64 h1;
    asm volatile(TG_LIMBS_MUL_1_PAIRS_ASM("add", "adc")
                 : [i] "+r"(i), [c] "+r"(carry), [l0] "=&r"(l0), [h0] "=&r"(h0), [l1] "=&r"(l1), [h1] "=&r"(h1)
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
    if (n <= 0)
        return 0;
    u64 carry = 0;
    if ((n & 1) != 0)
    {
        auto const p = cc::umul128(a[0], b);
        auto const d = cc::sub_with_borrow(r[0], p.lo);
        r[0] = d.value;
        carry = p.hi + d.borrow;
        ++r;
        ++a;
        --n;
        if (n == 0)
            return carry;
    }
    auto i = -i64(n);
    u64 l0;
    u64 h0;
    u64 l1;
    u64 h1;
    asm volatile(TG_LIMBS_MUL_1_PAIRS_ASM("sub", "sbb")
                 : [i] "+r"(i), [c] "+r"(carry), [l0] "=&r"(l0), [h0] "=&r"(h0), [l1] "=&r"(l1), [h1] "=&r"(h1)
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

namespace
{
/// floor((2^192 - 1) / d) - 2^64 for d = d1 * 2^64 + d0 with d1's top bit set: the reciprocal of a 3/2 division.
/// From Möller and Granlund, "Improved division by invariant integers" (IEEE Transactions on Computers, 2011), algorithms 2 and 6.
u64 reciprocal_3by2(u64 d1, u64 d0)
{
    // floor((2^128 - 1) / d1) - 2^64, then corrected for d0
    auto v = cc::udiv128({~u64(0), ~d1}, d1).quotient;
    auto p = d1 * v + d0;
    if (p < d0)
    {
        --v;
        if (p >= d1)
        {
            --v;
            p -= d1;
        }
        p -= d1;
    }
    auto const t = cc::umul128(d0, v);
    p += t.hi;
    if (p < t.hi)
    {
        --v;
        if (p >= d1 && (p > d1 || t.lo >= d0))
            --v;
    }
    return v;
}

struct div_3by2_result
{
    u64 quotient;
    u64 r1;
    u64 r0;
};

/// (n2, n1, n0) / (d1, d0) for (n2, n1) < (d1, d0) and v = reciprocal_3by2(d1, d0), by the same paper's algorithm 5.
/// Two products and at most one correction, where a hardware division costs tens of cycles.
CC_FORCE_INLINE div_3by2_result div_3by2(u64 n2, u64 n1, u64 n0, u64 d1, u64 d0, u64 v)
{
    auto const p = cc::umul128(n2, v);
    auto const s = cc::add_with_carry(p.lo, n1);
    auto q = p.hi + n2 + s.carry;
    auto const q0 = s.value;
    // the top two limbs of n - q * d, less d once more
    auto const e = cc::sub_with_borrow(n0, d0);
    auto r1 = n1 - d1 * q - d1 - e.borrow;
    auto const t = cc::umul128(d0, q);
    auto const f = cc::sub_with_borrow(e.value, t.lo);
    auto r0 = f.value;
    r1 = r1 - t.hi - f.borrow;
    ++q;
    // r1 >= q0 means q is one too large; branch-free, since it is taken about half the time
    auto const mask = u64(0) - u64(r1 >= q0);
    q += mask;
    auto const g = cc::add_with_carry(r0, mask & d0);
    r0 = g.value;
    r1 = r1 + (mask & d1) + g.carry;
    if (r1 >= d1) [[unlikely]]
    {
        if (r1 > d1 || r0 >= d0)
        {
            ++q;
            auto const h = cc::sub_with_borrow(r0, d0);
            r0 = h.value;
            r1 = r1 - d1 - h.borrow;
        }
    }
    return {.quotient = q, .r1 = r1, .r0 = r0};
}
} // namespace

void tg::impl::limbs_divrem_2(u64* q, u64* r, u64 const* a, i32 an, u64 const* b)
{
    CC_ASSERT(an >= 2 && b[1] != 0, "limbs_divrem_2 takes a normalized dividend of two limbs or more and a divisor of "
                                    "two");
    // the window is two limbs, kept in registers: no scratch, no submul, and the remainder is the last division's
    // a is shifted as it is read, each shift split in two so that s = 0 needs no branch
    auto const s = cc::count_leading_zeroes(b[1]);
    auto const d1 = (b[1] << s) | ((b[0] >> 1) >> (63 - s));
    auto const d0 = b[0] << s;
    auto const v = reciprocal_3by2(d1, d0);
    auto n1 = (a[an - 1] >> 1) >> (63 - s);
    auto n0 = (a[an - 1] << s) | ((a[an - 2] >> 1) >> (63 - s));
    for (auto j = an - 2; j >= 0; --j)
    {
        auto const below = j > 0 ? a[j - 1] : u64(0);
        auto const d = div_3by2(n1, n0, (a[j] << s) | ((below >> 1) >> (63 - s)), d1, d0, v);
        q[j] = d.quotient;
        n1 = d.r1;
        n0 = d.r0;
    }
    r[0] = (n0 >> s) | ((n1 << 1) << (63 - s));
    r[1] = n1 >> s;
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

    // each digit from the window's top three limbs over the divisor's top two, exact or one too large,
    // so a digit costs two products where Knuth's estimate costs a hardware division and up to two corrections
    auto const d1 = vn[bn - 1];
    auto const d0 = vn[bn - 2];
    auto const v = reciprocal_3by2(d1, d0);
    // the window un[j, j + bn] with its top limb held here rather than in un[j + bn]
    // the top starts below d1: it holds the bits a's shift moved out, fewer than the divisor's leading zeroes
    auto n1 = un[an];
    for (auto j = an - bn; j >= 0; --j)
    {
        u64 qhat;
        if (n1 == d1 && un[j + bn - 1] == d0) [[unlikely]]
        {
            // the one top the 3/2 division cannot take; then the digit is 2^64 - 1, exactly,
            // and the subtraction clears the top limb
            qhat = ~u64(0);
            (void)limbs_submul_1(un + j, vn, bn, qhat);
            n1 = un[j + bn - 1];
        }
        else
        {
            auto const d = div_3by2(n1, un[j + bn - 1], un[j + bn - 2], d1, d0, v);
            qhat = d.quotient;
            // the rest of the divisor times the digit, from the window below its top two, which the division left in r1 and r0
            auto const c = limbs_submul_1(un + j, vn, bn - 2, qhat);
            auto const s0 = cc::sub_with_borrow(d.r0, c);
            auto const s1 = cc::sub_with_borrow(d.r1, 0, s0.borrow);
            un[j + bn - 2] = s0.value;
            n1 = s1.value;
            if (s1.borrow != 0) [[unlikely]]
            {
                // one too large after all: the divisor added back, its top limb with the carry into n1
                --qhat;
                n1 += d1 + limbs_add(un + j, un + j, bn - 1, vn, bn - 1);
            }
        }
        q[j] = qhat;
    }
    un[bn - 1] = n1;

    if (s == 0)
        for (auto i = 0; i < bn; ++i)
            r[i] = un[i];
    else
        limbs_rshift(r, un, bn, s);
}
