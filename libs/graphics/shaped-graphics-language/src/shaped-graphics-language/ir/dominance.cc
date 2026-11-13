#include "dominance.hh"

using namespace sgl;

// Cooper, Harvey, Kennedy: "A Simple, Fast Dominance Algorithm".

sgl::ir::dominance::dominance(cc::span<u32 const> first, cc::span<u32 const> preds)
{
    auto const count = first.size() - 1;
    auto& idom = _idom;
    idom.resize_to_filled(count, -1);
    idom[0] = 0;
    auto const intersect = [&](i32 a, i32 b)
    {
        while (a != b)
        {
            while (a > b)
                a = idom[a];
            while (b > a)
                b = idom[b];
        }
        return a;
    };
    auto changed = true;
    while (changed)
    {
        changed = false;
        for (auto node = isize(1); node < count; ++node)
        {
            auto best = -1;
            for (auto k = first[node]; k < first[node + 1]; ++k)
            {
                auto const from = i32(preds[k]);
                if (idom[from] >= 0)
                    best = best < 0 ? from : intersect(best, from);
            }
            if (best >= 0 && idom[node] != best)
            {
                idom[node] = best;
                changed = true;
            }
        }
    }

    // children as a run per node, then one walk
    auto child_first = cc::vector<u32>::create_filled(count + 1, 0u);
    for (auto node = isize(1); node < count; ++node)
        if (idom[node] >= 0)
            ++child_first[idom[node] + 1];
    for (auto node = isize(0); node < count; ++node)
        child_first[node + 1] += child_first[node];
    auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(child_first));
    auto children = cc::vector<u32>::create_filled(isize(child_first[count]), 0u);
    for (auto node = isize(1); node < count; ++node)
        if (idom[node] >= 0)
            children[cursor[idom[node]]++] = u32(node);

    _entered.resize_to_filled(count, 1u);
    _left.resize_to_filled(count, 0u);
    auto clock = u32(0);
    struct frame
    {
        u32 node;
        u32 next;
    };
    auto stack = cc::vector<frame>();
    stack.push_back({.node = 0, .next = child_first[0]});
    _entered[0] = clock++;
    while (!stack.empty())
    {
        auto& top = stack.back();
        if (top.next < child_first[top.node + 1])
        {
            auto const child = children[top.next++];
            _entered[child] = clock++;
            stack.push_back({.node = child, .next = child_first[child]});
            continue;
        }
        _left[top.node] = clock++;
        stack.pop_back();
    }
}
