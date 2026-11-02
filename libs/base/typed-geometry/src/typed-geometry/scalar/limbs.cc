#include <clean-core/math/bit.hh>
#include <typed-geometry/scalar/impl/limbs.hh>

using namespace cc::primitive_defines;

u64 tg::impl::limbs_addmul_1(u64* r, u64 const* a, i32 n, u64 b)
{
    u64 carry = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const p = limb_mul_add(r[i], a[i], b, carry);
        r[i] = p.lo;
        carry = p.hi;
    }
    return carry;
}

namespace
{
/// Product scanning with both sizes known: each column of partial products summed in three registers and stored once, fully unrolled.
/// A generic column loop spends more on its bounds than on the products at these sizes.
template <int A, int B>
void mul_fixed(u64* r, u64 const* a, u64 const* b)
{
    u64 c0 = 0;
    u64 c1 = 0;
    u64 c2 = 0;
    for (auto k = 0; k < A + B - 1; ++k)
    {
        for (auto i = 0; i < A; ++i)
        {
            auto const j = k - i;
            if (j < 0 || j >= B)
                continue;
            auto const p = cc::umul128(a[i], b[j]);
            auto const s0 = cc::add_with_carry(c0, p.lo);
            auto const s1 = cc::add_with_carry(c1, p.hi, s0.carry);
            c0 = s0.value;
            c1 = s1.value;
            c2 += s1.carry;
        }
        r[k] = c0;
        c0 = c1;
        c1 = c2;
        c2 = 0;
    }
    r[A + B - 1] = c0;
}

/// The largest operand mul_fixed covers, in limbs: 256 bits.
constexpr i32 mul_fixed_limit = 4;

using mul_fixed_fn = void (*)(u64*, u64 const*, u64 const*);
constexpr mul_fixed_fn mul_fixed_table[mul_fixed_limit][mul_fixed_limit] = {
    {mul_fixed<1, 1>, mul_fixed<1, 2>, mul_fixed<1, 3>, mul_fixed<1, 4>},
    {mul_fixed<2, 1>, mul_fixed<2, 2>, mul_fixed<2, 3>, mul_fixed<2, 4>},
    {mul_fixed<3, 1>, mul_fixed<3, 2>, mul_fixed<3, 3>, mul_fixed<3, 4>},
    {mul_fixed<4, 1>, mul_fixed<4, 2>, mul_fixed<4, 3>, mul_fixed<4, 4>},
};


/// r[0, n) += a[0, n) * (b0 + b1 * 2^64), writing r[n] and returning the limb for r[n + 1].
/// Two rows in one pass, so r is loaded and stored once per two products.
u64 addmul_2(u64* r, u64 const* a, i32 n, u64 b0, u64 b1)
{
    // c0 is pending for the column at i, c1 for the one above it
    u64 c0 = 0;
    u64 c1 = 0;
    for (auto i = 0; i < n; ++i)
    {
        auto const t = tg::impl::limb_mul_add(r[i], a[i], b0, c0);
        r[i] = t.lo;
        auto const u = tg::impl::limb_mul_add(t.hi, a[i], b1, c1);
        c0 = u.lo;
        c1 = u.hi;
    }
    r[n] = c0;
    return c1;
}
} // namespace

void tg::impl::limbs_mul(u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    if (an <= mul_fixed_limit && bn <= mul_fixed_limit)
    {
        mul_fixed_table[an - 1][bn - 1](r, a, b);
        return;
    }
    // the first row writes r rather than adding into it, which saves clearing it; then the rows go two at a time
    r[an] = limbs_mul_1(r, a, an, b[0]);
    auto j = 1;
    for (; j + 1 < bn; j += 2)
        r[an + j + 1] = addmul_2(r + j, a, an, b[j], b[j + 1]);
    if (j < bn)
        r[an + j] = limbs_addmul_1(r + j, a, an, b[j]);
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
        u64 borrow = 0;
        u64 carry = 0;
        for (auto i = 0; i < bn; ++i)
        {
            auto const p = limb_mul_add(0, qhat, vn[i], carry);
            carry = p.hi;
            auto const d = cc::sub_with_borrow(un[i + j], p.lo, borrow);
            un[i + j] = d.value;
            borrow = d.borrow;
        }
        auto const top = cc::sub_with_borrow(un[j + bn], carry, borrow);
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
