#include "bvh8.hh"

#include <clean-core/algorithm/sort.hh>

namespace
{
constexpr cimd::f32 empty_lo = 3.0e38f;
constexpr cimd::f32 empty_hi = -3.0e38f;

bvh8_box bounds_of(cc::span<bvh8_box const> prims, cc::span<int const> ids)
{
    auto b = bvh8_box{.min = {empty_lo, empty_lo, empty_lo}, .max = {empty_hi, empty_hi, empty_hi}};
    for (auto const id : ids)
        for (auto a = 0; a < 3; ++a)
        {
            b.min[a] = prims[id].min[a] < b.min[a] ? prims[id].min[a] : b.min[a];
            b.max[a] = prims[id].max[a] > b.max[a] ? prims[id].max[a] : b.max[a];
        }
    return b;
}

void set_slot(bvh8_node& n, int slot, bvh8_box const& b, cimd::i32 child)
{
    for (auto a = 0; a < 3; ++a)
    {
        n.lo[a].lanes[slot] = b.min[a];
        n.hi[a].lanes[slot] = b.max[a];
    }
    n.child.lanes[slot] = child;
}

/// Builds the node for `ids` into `tree.nodes[at]`, which the caller has already appended.
void build_into(bvh8& tree, int at, cc::span<bvh8_box const> prims, cc::span<int> ids)
{
    auto node = bvh8_node{};
    for (auto slot = 0; slot < 8; ++slot)
        set_slot(node, slot, bvh8_box{.min = {empty_lo, empty_lo, empty_lo}, .max = {empty_hi, empty_hi, empty_hi}}, 0);

    if (ids.size() <= 8)
    {
        for (auto slot = 0; slot < int(ids.size()); ++slot)
            set_slot(node, slot, prims[ids[slot]], ~ids[slot]);
        tree.nodes[at] = node;
        return;
    }

    // Median split along the longest axis of the boxes' bounds, into eight equal runs by centroid.
    auto const all = bounds_of(prims, ids);
    auto axis = 0;
    for (auto a = 1; a < 3; ++a)
        if (all.max[a] - all.min[a] > all.max[axis] - all.min[axis])
            axis = a;
    cc::sort(ids, [&](int l, int r)
             { return prims[l].min[axis] + prims[l].max[axis] < prims[r].min[axis] + prims[r].max[axis]; });

    auto const n = ids.size();
    for (auto slot = 0; slot < 8; ++slot)
    {
        auto const begin = n * slot / 8;
        auto const end = n * (slot + 1) / 8;
        if (begin == end)
            continue;
        auto const run = ids.subspan({.start = begin, .end = end});
        auto const child = int(tree.nodes.size());
        tree.nodes.push_back(bvh8_node{});
        build_into(tree, child, prims, run);
        set_slot(node, slot, bounds_of(prims, run), child);
    }
    tree.nodes[at] = node;
}
} // namespace

bvh8 build_bvh8(cc::span<bvh8_box const> prims)
{
    auto tree = bvh8();
    if (prims.empty())
        return tree;
    auto ids = cc::vector<int>();
    for (auto i = 0; i < int(prims.size()); ++i)
        ids.push_back(i);
    tree.nodes.push_back(bvh8_node{});
    build_into(tree, 0, prims, ids);
    return tree;
}
