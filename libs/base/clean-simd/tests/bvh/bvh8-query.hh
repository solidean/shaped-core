#pragma once

#include "bvh8.hh"

#include <clean-core/math/bit.hh>
#include <clean-simd/all.hh>
#include <clean-simd/dispatch.hh>

/// Every primitive whose box overlaps `q`, written to `out` up to `cap`; returns how many overlap, which may exceed `cap`.
/// Dispatched: one instantiation per kernel, chosen per CPU.
template <class K>
int bvh8_query(bvh8_view tree, bvh8_box q, cimd::i32* out, int cap)
{
    using f32x8 = cimd::f32x8<K>;

    if (tree.node_count == 0)
        return 0;

    f32x8 const qlo[3] = {f32x8(q.min[0]), f32x8(q.min[1]), f32x8(q.min[2])};
    f32x8 const qhi[3] = {f32x8(q.max[0]), f32x8(q.max[1]), f32x8(q.max[2])};

    cimd::i32 stack[256];
    auto top = 0;
    stack[top++] = 0;
    auto found = 0;

    while (top > 0)
    {
        auto const& n = tree.nodes[stack[--top]];
        auto const hit = (f32x8(n.lo[0]) <= qhi[0]) & (f32x8(n.hi[0]) >= qlo[0]) //
                       & (f32x8(n.lo[1]) <= qhi[1]) & (f32x8(n.hi[1]) >= qlo[1]) //
                       & (f32x8(n.lo[2]) <= qhi[2]) & (f32x8(n.hi[2]) >= qlo[2]);
        if (hit.none())
            continue;

        for (auto bits = hit.bits(); bits != 0; bits &= bits - 1)
        {
            auto const c = n.child.lanes[cc::count_trailing_zeroes(bits)];
            if (c >= 0)
                stack[top++] = c;
            else if (found++ < cap)
                out[found - 1] = ~c;
        }
    }
    return found;
}

CIMD_DISPATCH_DECLARE(bvh8_query_dispatched, bvh8_query);
