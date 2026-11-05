#include <typed-geometry/scalar/float_expansion/float_expansion.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace
{
using namespace cc::primitive_defines;
using tg::impl::float_expansion_invalid;

/// Whether a sum chain stayed in range: an overflow makes its running total infinite or NaN, and both stay so.
bool is_finite(f64 q)
{
    return q - q == 0.0;
}

/// h = e + b: Shewchuk's grow_expansion_zeroelim, a two_sum per component and no merge.
/// h may alias e: each component is read before the one written at its index or below.
/// A nonadjacent e gives a nonadjacent h, so growing from single doubles keeps the merges' precondition.
/// Invalid where a sum overflowed; e must be valid.
i32 grow(f64 const* e, i32 en, f64 b, f64* h)
{
    auto q = b;
    auto hn = 0;
    for (auto i = 0; i < en; ++i)
    {
        auto const enow = e[i];
        auto const x = q + enow;
        auto const bv = x - q;
        auto const av = x - bv;
        auto const y = (q - av) + (enow - bv);
        q = x;
        if (y != 0.0)
            h[hn++] = y;
    }
    if (!is_finite(q)) [[unlikely]]
        return float_expansion_invalid;
    if (q != 0.0)
        h[hn++] = q;
    return hn;
}

/// h = e + s f, s = 1 or -1, for nonempty e and f: Shewchuk's fast_expansion_sum_zeroelim.
i32 merge(f64 const* e, i32 en, f64 const* f, i32 fn, f64 s, f64* h)
{
    auto ei = 0;
    auto fi = 0;
    auto enow = e[0];
    auto fnow = s * f[0];
    // the two inputs merged by magnitude: (fnow > enow) == (fnow > -enow) is |enow| < |fnow|
    f64 q;
    if ((fnow > enow) == (fnow > -enow))
    {
        q = enow;
        enow = ++ei < en ? e[ei] : 0.0;
    }
    else
    {
        q = fnow;
        fnow = ++fi < fn ? s * f[fi] : 0.0;
    }
    auto hn = 0;
    if (ei < en && fi < fn)
    {
        // the first merge step is a fast two_sum: the next component is at least as large as q
        f64 x;
        f64 y;
        if ((fnow > enow) == (fnow > -enow))
        {
            x = enow + q;
            y = q - (x - enow);
            enow = ++ei < en ? e[ei] : 0.0;
        }
        else
        {
            x = fnow + q;
            y = q - (x - fnow);
            fnow = ++fi < fn ? s * f[fi] : 0.0;
        }
        q = x;
        if (y != 0.0)
            h[hn++] = y;
        while (ei < en && fi < fn)
        {
            f64 b;
            if ((fnow > enow) == (fnow > -enow))
            {
                b = enow;
                enow = ++ei < en ? e[ei] : 0.0;
            }
            else
            {
                b = fnow;
                fnow = ++fi < fn ? s * f[fi] : 0.0;
            }
            x = q + b;
            auto const bv = x - q;
            auto const av = x - bv;
            y = (q - av) + (b - bv);
            q = x;
            if (y != 0.0)
                h[hn++] = y;
        }
    }
    // the rest of whichever input is left, by two_sum
    while (ei < en)
    {
        auto const x = q + enow;
        auto const bv = x - q;
        auto const av = x - bv;
        auto const y = (q - av) + (enow - bv);
        enow = ++ei < en ? e[ei] : 0.0;
        q = x;
        if (y != 0.0)
            h[hn++] = y;
    }
    while (fi < fn)
    {
        auto const x = q + fnow;
        auto const bv = x - q;
        auto const av = x - bv;
        auto const y = (q - av) + (fnow - bv);
        fnow = ++fi < fn ? s * f[fi] : 0.0;
        q = x;
        if (y != 0.0)
            h[hn++] = y;
    }
    if (!is_finite(q)) [[unlikely]]
        return float_expansion_invalid;
    if (q != 0.0)
        h[hn++] = q;
    return hn;
}

/// h = b e, b nonzero: Shewchuk's scale_expansion_zeroelim; h holds 2 en.
i32 scale(f64 const* e, i32 en, f64 b, f64* h)
{
    f64 p[2];
    auto const pn = tg::impl::float_expansion_two_product(e[0], b, p);
    if (pn == float_expansion_invalid) [[unlikely]]
        return float_expansion_invalid;
    auto hn = 0;
    if (pn == 2)
        h[hn++] = p[0];
    auto q = p[pn - 1];
    for (auto i = 1; i < en; ++i)
    {
        auto const pi = tg::impl::float_expansion_two_product(e[i], b, p);
        if (pi == float_expansion_invalid) [[unlikely]]
            return float_expansion_invalid;
        auto const p0 = pi == 2 ? p[0] : 0.0;
        auto const p1 = p[pi - 1];
        // q + p0 by two_sum, then its high part + p1 by fast two_sum, p1 being the larger
        auto const x = q + p0;
        auto const bv = x - q;
        auto const av = x - bv;
        auto y = (q - av) + (p0 - bv);
        if (y != 0.0)
            h[hn++] = y;
        q = p1 + x;
        y = x - (q - p1);
        if (y != 0.0)
            h[hn++] = y;
    }
    if (!is_finite(q)) [[unlikely]]
        return float_expansion_invalid;
    if (q != 0.0)
        h[hn++] = q;
    return hn;
}
} // namespace

i32 tg::impl::float_expansion_sum(f64 const* e, i32 en, f64 const* f, i32 fn, bool negate_f, f64* h)
{
    auto const s = negate_f ? -1.0 : 1.0;
    if (fn == 0)
    {
        for (auto i = 0; i < en; ++i)
            h[i] = e[i];
        return en;
    }
    if (en == 0)
    {
        for (auto i = 0; i < fn; ++i)
            h[i] = s * f[i];
        return fn;
    }
    // a short side is grown in one component at a time, which skips the merge's comparisons
    if (fn <= 2)
    {
        auto const hn = grow(e, en, s * f[0], h);
        return fn == 2 && hn != float_expansion_invalid ? grow(h, hn, s * f[1], h) : hn;
    }
    if (en <= 2)
    {
        // f's components negated as they are grown over, then e's added
        for (auto i = 0; i < fn; ++i)
            h[i] = s * f[i];
        auto const hn = grow(h, fn, e[0], h);
        return en == 2 && hn != float_expansion_invalid ? grow(h, hn, e[1], h) : hn;
    }
    return merge(e, en, f, fn, s, h);
}

i32 tg::impl::float_expansion_product(f64 const* e, i32 en, f64 const* f, i32 fn, f64* h)
{
    // scale by the shorter side: fewer, longer merges
    if (fn > en)
    {
        cc::swap(e, f);
        cc::swap(en, fn);
    }
    if (fn == 1)
        return scale(e, en, f[0], h);

    // h accumulates the partial products, ping-ponging with t; s holds one scaled e
    auto const total = 2 * en * fn;
    constexpr i32 stack_components = 128;
    f64 stack[stack_components];
    f64* heap = nullptr;
    u32 heap_class = 0;
    f64* scratch = stack;
    if (total + 2 * en > stack_components)
        scratch = heap = float_expansion_allocate(total + 2 * en, heap_class);
    auto* t = scratch;
    auto* const s = scratch + total;

    auto* cur = h;
    auto* next = t;
    auto n = scale(e, en, f[0], cur);
    for (auto i = 1; i < fn && n != float_expansion_invalid; ++i)
    {
        auto const sn = scale(e, en, f[i], s);
        n = sn == float_expansion_invalid ? sn : float_expansion_sum(cur, n, s, sn, false, next);
        cc::swap(cur, next);
    }
    if (cur != h && n != float_expansion_invalid)
        for (auto i = 0; i < n; ++i)
            h[i] = cur[i];
    if (heap)
        float_expansion_free(heap, heap_class);
    return n;
}

i32 tg::impl::float_expansion_from_limbs(u64 const* limbs, i32 n, bool negative, f64* h)
{
    // each limb's halves are doubles exactly, grown in from the lowest so the result stays nonadjacent
    auto const sign = negative ? -1.0 : 1.0;
    auto hn = 0;
    for (auto i = 0; i < n && hn != float_expansion_invalid; ++i)
    {
        auto const lo = f64(limbs[i] & 0xffffffffu);
        auto const hi = f64(limbs[i] >> 32);
        if (lo != 0.0)
            hn = grow(h, hn, sign * tg::scale_by_pow2(lo, 64 * i), h);
        if (hi != 0.0 && hn != float_expansion_invalid)
            hn = grow(h, hn, sign * tg::scale_by_pow2(hi, 64 * i + 32), h);
    }
    return hn;
}

f64 tg::impl::float_expansion_approximate(f64 const* e, i32 en)
{
    auto r = 0.0;
    for (auto i = 0; i < en; ++i)
        r += e[i];
    return r;
}
