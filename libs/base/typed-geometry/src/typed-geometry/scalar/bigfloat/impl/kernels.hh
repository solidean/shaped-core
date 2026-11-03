#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/math/wide_arith.hh>
#include <clean-core/memory/node_allocation.hh>
#include <typed-geometry/scalar/fwd.hh>

// The kernels behind tg::bigfloat_sbo<K>: non-template, over a view of a mantissa, so every K shares one copy.
// Users include scalar/bigfloat/bigfloat.hh.

namespace tg::impl
{
/// The magnitude limbs * 2^exponent, limbs least significant first.
/// A value's view is trimmed: size 0 is zero, and otherwise its lowest and highest limbs are nonzero.
struct bigfloat_view
{
    u64 const* limbs = nullptr;
    i32 size = 0;
    i32 exponent = 0;
};

/// A kernel's result in the caller's limbs: size limbs, times 2^exponent, trimmed by bigfloat_trim.
struct bigfloat_raw
{
    i32 size = 0;
    i32 exponent = 0;
    bool negative = false;
};

/// The limbs |a| + |b| or |a| - |b| needs once aligned at the smaller exponent, the carry included; both nonzero.
/// An exponent gap costs limbs: the window is dense from the lower value's bottom to the higher value's top.
[[nodiscard]] inline i32 bigfloat_sum_limbs(bigfloat_view a, bigfloat_view b)
{
    auto const e = a.exponent < b.exponent ? a.exponent : b.exponent;
    auto const limbs_at = [e](bigfloat_view x)
    {
        auto const s = i64(x.exponent) - e;
        return (s >> 6) + x.size + i64((s & 63) != 0);
    };
    auto const la = limbs_at(a);
    auto const lb = limbs_at(b);
    auto const n = (la > lb ? la : lb) + 1;
    CC_ASSERT(n < (i64(1) << 26), "a bigfloat holds fewer than 2^26 limbs");
    return i32(n);
}

/// Drops zero limbs at both ends, moving the rest down; a result of no limbs is zero, with exponent 0.
[[nodiscard]] inline bigfloat_raw bigfloat_trim(u64* limbs, bigfloat_raw r)
{
    auto n = r.size;
    while (n > 0 && limbs[n - 1] == 0)
        --n;
    if (n == 0)
        return {};
    auto z = 0;
    while (limbs[z] == 0)
        ++z;
    if (z > 0)
    {
        for (auto i = z; i < n; ++i)
            limbs[i - z] = limbs[i];
        n -= z;
    }
    return {.size = n, .exponent = r.exponent + 64 * z, .negative = r.negative};
}

/// bigfloat_trim for a fixed window of N limbs, by selects rather than loops: which limbs are zero is data, so a branch on it mispredicts.
/// Only a zero lowest limb branches, which a sum or product of trimmed values rarely has.
template <int N>
[[nodiscard]] CC_FORCE_INLINE bigfloat_raw bigfloat_trim_window(u64* w, i32 exponent, bool negative)
{
    auto n = 0;
    for (auto i = 0; i < N; ++i)
        n = w[i] != 0 ? i + 1 : n;
    if (w[0] == 0) [[unlikely]]
        return bigfloat_trim(w, {.size = n, .exponent = exponent, .negative = negative});
    return {.size = n, .exponent = exponent, .negative = negative};
}

/// Whether bigfloat_add_small applies: both of at most 2 limbs, their exponents less than 64 apart.
[[nodiscard]] CC_FORCE_INLINE bool bigfloat_is_small_sum(bigfloat_view a, bigfloat_view b)
{
    auto const gap = i64(a.exponent) - b.exponent;
    return a.size <= 2 && b.size <= 2 && gap > -64 && gap < 64;
}

/// bigfloat_add_kernel for bigfloat_is_small_sum operands of at most L limbs, in a window of L + 1: the shifted higher value is below 2^(64L + 63).
/// L = 1 is every difference of two doubles of similar magnitude, which is why it has a window of its own.
/// Branch-free past the operands' sizes: the sign of a difference is data, and a branch on it mispredicts half the time.
template <int L>
[[nodiscard]] CC_FORCE_INLINE bigfloat_raw bigfloat_add_small(u64* out, bigfloat_view a, bigfloat_view b, bool subtract)
{
    constexpr int w = L + 1;
    auto const a_high = a.exponent >= b.exponent;
    auto const h = a_high ? a : b;
    auto const l = a_high ? b : a;
    auto const d = h.exponent - l.exponent;
    u64 hl[L];
    u64 y[w];
    for (auto i = 0; i < L; ++i)
    {
        hl[i] = i < h.size ? h.limbs[i] : u64(0);
        y[i] = i < l.size ? l.limbs[i] : u64(0);
    }
    y[L] = 0;
    // (v >> 1) >> (63 - d) is v >> (64 - d), and 0 rather than UB when d is 0
    u64 x[w];
    for (auto i = 0; i < w; ++i)
        x[i] = (i < L ? hl[i] << d : u64(0)) | (i > 0 ? (hl[i - 1] >> 1) >> (63 - d) : u64(0));

    // x - y is x + ~y + 1, so one carry chain serves both, and its carry out is x >= y
    auto const flip = u64(0) - u64(subtract);
    u64 carry = u64(subtract);
    for (auto i = 0; i < w; ++i)
    {
        auto const s = cc::add_with_carry(x[i], y[i] ^ flip, carry);
        out[i] = s.value;
        carry = s.carry;
    }
    // |h| < |l|: the window holds 2^(64w) - (|l| - |h|), negated back by a mask
    auto const borrow = u64(subtract) & (carry ^ 1);
    auto const mask = u64(0) - borrow;
    carry = borrow;
    for (auto i = 0; i < w; ++i)
    {
        auto const s = cc::add_with_carry(out[i] ^ mask, 0, carry);
        out[i] = s.value;
        carry = s.carry;
    }
    // |a| - |b| is |h| - |l|, or its negation when a is the lower value; a zero result trims to size 0 whatever this says
    return bigfloat_trim_window<w>(out, l.exponent, subtract && a_high == (borrow != 0));
}

/// out = |a| * |b| for operands of at most 2 limbs, into 4 limbs.
[[nodiscard]] CC_FORCE_INLINE bigfloat_raw bigfloat_mul_small(u64* out4, bigfloat_view a, bigfloat_view b)
{
    auto const e = i64(a.exponent) + b.exponent;
    CC_ASSERT(e == i64(i32(e)), "bigfloat exponent overflow");
    if (a.size == 1 && b.size == 1)
    {
        auto const p = cc::umul128(a.limbs[0], b.limbs[0]);
        out4[0] = p.lo;
        out4[1] = p.hi;
        // the caller copies a fixed prefix of the window, so every limb is written
        out4[2] = 0;
        out4[3] = 0;
        return bigfloat_trim_window<2>(out4, i32(e), false);
    }
    u64 const x[2] = {a.limbs[0], a.size > 1 ? a.limbs[1] : u64(0)};
    u64 const y[2] = {b.limbs[0], b.size > 1 ? b.limbs[1] : u64(0)};
    for (auto i = 0; i < 4; ++i)
        out4[i] = 0;
    for (auto i = 0; i < 2; ++i)
    {
        u64 carry = 0;
        for (auto j = 0; j < 2; ++j)
        {
            auto const p = cc::umul128(x[i], y[j]);
            auto const s1 = cc::add_with_carry(p.lo, out4[i + j]);
            auto const s2 = cc::add_with_carry(s1.value, carry);
            out4[i + j] = s2.value;
            carry = p.hi + s1.carry + s2.carry;
        }
        out4[i + 2] = carry;
    }
    return bigfloat_trim_window<4>(out4, i32(e), false);
}

/// out = |a| + |b|, or |a| - |b| when subtract, negative when |a| < |b|; both nonzero.
/// out must hold bigfloat_sum_limbs(a, b) limbs and must not overlap either operand.
[[nodiscard]] bigfloat_raw bigfloat_add_kernel(u64* out, bigfloat_view a, bigfloat_view b, bool subtract);

/// out = |a| * |b|; both nonzero, out holds a.size + b.size limbs and overlaps neither operand.
[[nodiscard]] bigfloat_raw bigfloat_mul_kernel(u64* out, bigfloat_view a, bigfloat_view b);

/// The sign of |a| - |b|, without computing it; both nonzero.
[[nodiscard]] int bigfloat_compare_kernel(bigfloat_view a, bigfloat_view b);

/// The magnitude rounded to nearest, ties to even, and negated when negative.
/// Correctly rounded unless the result is subnormal, where it may round twice; every double converts back exactly.
[[nodiscard]] f64 bigfloat_to_f64(bigfloat_view v, bool negative);

/// limbs from cc's node allocator, at least the requested count; cap_class is the node class to free them with.
[[nodiscard]] inline u64* bigfloat_allocate(i32 limbs, u32& cap_class)
{
    auto const idx = cc::node_class_index_from_size_and_align(isize(limbs) * 8, 8);
    CC_ASSERT(int(idx) < 32, "a bigfloat's capacity class fits 5 bits");
    cap_class = u32(idx);
    return reinterpret_cast<u64*>(cc::default_node_allocator().allocate_node_bytes(idx, isize(1) << int(idx), 8)); // NOLINT
}

inline void bigfloat_free(u64* limbs, u32 cap_class)
{
    cc::node_allocation_free(reinterpret_cast<byte*>(limbs), cc::node_class_index(cap_class)); // NOLINT
}

/// The limbs a capacity class holds; class 0 is the inline storage, which the caller knows the size of.
[[nodiscard]] constexpr i32 bigfloat_class_limbs(u32 cap_class)
{
    return i32((u64(1) << cap_class) / 8);
}
} // namespace tg::impl
