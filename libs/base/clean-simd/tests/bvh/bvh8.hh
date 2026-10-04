#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-simd/simd.hh>

// An 8-wide BVH, the layout clean-simd exists for: every node holds its eight children as planes of eight floats.
// The nodes store cimd::storage, so one tree is queried by whichever kernel the CPU picked.

struct bvh8_box
{
    cimd::f32 min[3] = {};
    cimd::f32 max[3] = {};
};

struct bvh8_node
{
    cimd::f32x8_storage lo[3]; // per axis, the eight children's minima
    cimd::f32x8_storage hi[3]; // and maxima; an empty slot has lo > hi, so it never overlaps
    cimd::i32x8_storage child; // >= 0: a node index; < 0: ~primitive index
};

/// What a query reads: plain pointers, so the dispatched code instantiates nothing of cc::vector in a kernel TU.
struct bvh8_view
{
    bvh8_node const* nodes = nullptr;
    int node_count = 0;
};

struct bvh8
{
    cc::vector<bvh8_node> nodes; // nodes[0] is the root
    [[nodiscard]] bvh8_view view() const { return {.nodes = nodes.data(), .node_count = int(nodes.size())}; }
};

/// Median splits along the longest axis, eight children per node, one primitive per leaf slot.
[[nodiscard]] bvh8 build_bvh8(cc::span<bvh8_box const> prims);
