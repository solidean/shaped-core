#pragma once

#include <clean-core/container/vector.hh>
#include <shader-analysis/metrics.hh>

// One function as every format's reader hands it to the measurements: blocks and the instructions in them, each
// value numbered densely, so liveness and the chains are written once for SPIR-V and DXIL alike.

namespace shan::impl
{
struct graph_instruction
{
    op_kind kind = op_kind::other;
    /// The value it gives, or -1.
    i32 result = -1;
    /// A range of `graph::operands`: the values it reads, a phi's not among them.
    u32 first = 0;
    u32 count = 0;
    /// A range of `graph::phi_values` and `graph::phi_blocks`, for a phi.
    u32 phi_first = 0;
    u32 phi_count = 0;
    /// Whether it runs, and so counts among `instructions` and the kinds: no merge, label or debug line.
    bool counts = true;
};

struct graph_block
{
    u32 first = 0;
    u32 end = 0;
    cc::vector<i32> successors;
};

struct graph
{
    cc::vector<graph_block> blocks;
    cc::vector<graph_instruction> instructions;
    cc::vector<i32> operands;
    cc::vector<i32> phi_values;
    cc::vector<i32> phi_blocks;
    /// Each value's 32-bit words.
    cc::vector<i32> widths;
};

/// The counts, the live words and the critical path of `g`, into `f`; the format fills loops, selections, calls and
/// variables itself.
void measure(graph const& g, function_metrics& f);

/// The edges that go back to a block dominating their source: one per loop's latch, in a format that states no merges.
[[nodiscard]] i32 back_edges(graph const& g);

/// `f` added into `total`: counts summed, the live words and the critical path the worst.
void accumulate(function_metrics const& f, function_metrics& total);
} // namespace shan::impl
