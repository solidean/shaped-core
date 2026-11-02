#include <clean-core/math/bit.hh>
#include <clean-core/math/wide_arith.hh>
#include <typed-geometry/scalar/bigfloat/bigfloat.hh>
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace
{
using namespace cc::primitive_defines;
using tg::impl::bigfloat_raw;
using tg::impl::bigfloat_view;

/// x * 2^(64 * word + bit), read one limb at a time; a position outside x reads 0.
struct shifted
{
    u64 const* limbs;
    i64 size;
    i64 word;
    int bit;

    [[nodiscard]] u64 at(i64 i) const
    {
        auto const j = i - word;
        // j < 0 wraps to a huge unsigned value, so one comparison checks both ends
        auto const hi = u64(j) < u64(size) ? limbs[j] : 0;
        if (bit == 0)
            return hi;
        auto const lo = u64(j - 1) < u64(size) ? limbs[j - 1] : 0;
        return (hi << bit) | (lo >> (64 - bit));
    }

    /// One past the highest limb that can be nonzero.
    [[nodiscard]] i64 end() const { return word + size + i64(bit != 0); }
};

/// x aligned at exponent e, which must not exceed x's.
[[nodiscard]] shifted aligned(bigfloat_view x, i64 e)
{
    auto const s = i64(x.exponent) - e;
    return {.limbs = x.limbs, .size = x.size, .word = s >> 6, .bit = int(s & 63)};
}

[[nodiscard]] i32 product_exponent(bigfloat_view a, bigfloat_view b)
{
    auto const e = i64(a.exponent) + b.exponent;
    CC_ASSERT(e == i64(i32(e)), "bigfloat exponent overflow");
    return i32(e);
}

/// out[0, a.size + b.size) = |a| * |b|, untrimmed; out overlaps neither operand.
void mul_limbs(u64* out, bigfloat_view a, bigfloat_view b)
{
    for (auto i = 0; i < a.size + b.size; ++i)
        out[i] = 0;
    for (auto i = 0; i < a.size; ++i)
    {
        u64 carry = 0;
        auto const ai = a.limbs[i];
        for (auto j = 0; j < b.size; ++j)
        {
            auto const p = tg::impl::mul_add(out[i + j], ai, b.limbs[j], carry);
            out[i + j] = p.lo;
            carry = p.hi;
        }
        out[i + b.size] = carry;
    }
}

/// limbs[0, n) negated in two's complement.
void negate_limbs(u64* limbs, i64 n)
{
    u64 carry = 1;
    for (auto i = 0; i < n; ++i)
    {
        auto const s = cc::add_with_carry(~limbs[i], 0, carry);
        limbs[i] = s.value;
        carry = s.carry;
    }
}

[[nodiscard]] bool is_zero_term(tg::bigfloat_summand const& t)
{
    return t.a.size == 0 || (t.has_b && t.b.size == 0);
}

[[nodiscard]] i64 term_exponent(tg::bigfloat_summand const& t)
{
    return i64(t.a.exponent) + (t.has_b ? t.b.exponent : 0);
}
} // namespace

bigfloat_raw tg::impl::bigfloat_add_kernel(u64* out, bigfloat_view a, bigfloat_view b, bool subtract)
{
    auto const e = a.exponent < b.exponent ? a.exponent : b.exponent;
    auto const x = aligned(a, e);
    auto const y = aligned(b, e);
    auto const n = x.end() > y.end() ? x.end() : y.end();
    if (!subtract)
    {
        u64 carry = 0;
        for (auto i = i64(0); i < n; ++i)
        {
            auto const s = cc::add_with_carry(x.at(i), y.at(i), carry);
            out[i] = s.value;
            carry = s.carry;
        }
        out[n] = carry;
        return bigfloat_trim(out, {.size = i32(n + 1), .exponent = e, .negative = false});
    }

    u64 borrow = 0;
    for (auto i = i64(0); i < n; ++i)
    {
        auto const s = cc::sub_with_borrow(x.at(i), y.at(i), borrow);
        out[i] = s.value;
        borrow = s.borrow;
    }
    // a borrow out means |a| < |b|, and out holds 2^(64n) - (|b| - |a|)
    if (borrow != 0)
        negate_limbs(out, n);
    return bigfloat_trim(out, {.size = i32(n), .exponent = e, .negative = borrow != 0});
}

bigfloat_raw tg::impl::bigfloat_mul_kernel(u64* out, bigfloat_view a, bigfloat_view b)
{
    mul_limbs(out, a, b);
    return bigfloat_trim(out, {.size = a.size + b.size, .exponent = product_exponent(a, b), .negative = false});
}

int tg::impl::bigfloat_compare_kernel(bigfloat_view a, bigfloat_view b)
{
    // the position of the top bit decides, unless the two agree
    auto const top
        = [](bigfloat_view v) { return i64(v.exponent) + 64 * i64(v.size - 1) + cc::bit_width(v.limbs[v.size - 1]); };
    auto const ta = top(a);
    auto const tb = top(b);
    if (ta != tb)
        return ta < tb ? -1 : 1;

    auto const e = a.exponent < b.exponent ? a.exponent : b.exponent;
    auto const x = aligned(a, e);
    auto const y = aligned(b, e);
    for (auto i = (x.end() > y.end() ? x.end() : y.end()) - 1; i >= 0; --i)
    {
        auto const xi = x.at(i);
        auto const yi = y.at(i);
        if (xi != yi)
            return xi < yi ? -1 : 1;
    }
    return 0;
}

f64 tg::impl::bigfloat_to_f64(bigfloat_view v, bool negative)
{
    if (v.size == 0)
        return 0.0;
    auto const n = v.size;
    auto const top = v.limbs[n - 1];
    auto const lz = cc::count_leading_zeroes(top);
    auto chunk = top << lz;
    auto sticky = false;
    if (n >= 2)
    {
        auto const next = v.limbs[n - 2];
        if (lz != 0)
            chunk |= next >> (64 - lz);
        sticky = (lz == 0 ? next : next << lz) != 0;
        for (auto i = 0; i < n - 2 && !sticky; ++i)
            sticky = v.limbs[i] != 0;
    }
    // the sticky bit sits below the 53 bits the conversion keeps, so it only breaks ties
    chunk |= u64(sticky);
    auto const scale = i64(v.exponent) + 64 * i64(n - 1) - lz;
    auto const r = tg::scale_by_pow2(f64(chunk), int(scale));
    return negative ? -r : r;
}

i32 tg::impl::bigfloat_msum_limbs(cc::span<bigfloat_summand const> terms)
{
    auto any = false;
    i64 lo = 0;
    i64 hi = 0;
    for (auto const& t : terms)
    {
        if (is_zero_term(t))
            continue;
        auto const e = term_exponent(t);
        auto const top = e + 64 * i64(t.a.size + (t.has_b ? t.b.size : 0));
        lo = any && lo < e ? lo : e;
        hi = any && hi > top ? hi : top;
        any = true;
    }
    if (!any)
        return 0;
    // one more limb for the sign and the carries, enough for fewer than 2^63 summands
    auto const n = (hi - lo + 63) / 64 + 1;
    CC_ASSERT(n < (i64(1) << 26), "a bigfloat holds fewer than 2^26 limbs");
    return i32(n);
}

bigfloat_raw tg::impl::bigfloat_msum_kernel(u64* out, i32 n, cc::span<bigfloat_summand const> terms)
{
    auto any = false;
    i64 lo = 0;
    for (auto const& t : terms)
        if (!is_zero_term(t))
        {
            auto const e = term_exponent(t);
            lo = any && lo < e ? lo : e;
            any = true;
        }

    for (auto i = 0; i < n; ++i)
        out[i] = 0;

    // products of up to 16 limbs, which covers every product of two inline values, need no allocation
    u64 scratch[16];
    for (auto const& t : terms)
    {
        if (is_zero_term(t))
            continue;

        auto p = t.a;
        u64* heap = nullptr;
        u32 heap_class = 0;
        if (t.has_b)
        {
            auto const pn = t.a.size + t.b.size;
            auto* buf = scratch;
            if (pn > 16)
                buf = heap = bigfloat_allocate(pn, heap_class);
            mul_limbs(buf, t.a, t.b);
            p = {.limbs = buf, .size = pn, .exponent = product_exponent(t.a, t.b)};
        }

        auto const s = aligned(p, lo);
        auto const end = s.end();
        if (!t.negative)
        {
            u64 carry = 0;
            auto i = s.word;
            for (; i < end; ++i)
            {
                auto const r = cc::add_with_carry(out[i], s.at(i), carry);
                out[i] = r.value;
                carry = r.carry;
            }
            for (; carry != 0 && i < n; ++i)
            {
                auto const r = cc::add_with_carry(out[i], 0, carry);
                out[i] = r.value;
                carry = r.carry;
            }
        }
        else
        {
            u64 borrow = 0;
            auto i = s.word;
            for (; i < end; ++i)
            {
                auto const r = cc::sub_with_borrow(out[i], s.at(i), borrow);
                out[i] = r.value;
                borrow = r.borrow;
            }
            for (; borrow != 0 && i < n; ++i)
            {
                auto const r = cc::sub_with_borrow(out[i], 0, borrow);
                out[i] = r.value;
                borrow = r.borrow;
            }
        }

        if (heap != nullptr)
            bigfloat_free(heap, heap_class);
    }

    auto const negative = (out[n - 1] >> 63) != 0;
    if (negative)
        negate_limbs(out, n);
    return bigfloat_trim(out, {.size = n, .exponent = i32(lo), .negative = negative});
}
