#pragma once

#include <clean-core/container/vector.hh>
#include <shader-analysis/cost.hh>
#include <shader-analysis/metrics.hh>

// One function as every format's reader hands it to the measurements: blocks and the instructions in them, each
// value numbered densely, so liveness and the chains are written once for SPIR-V and DXIL alike.

namespace shan::impl
{
/// Where a value's uniformity comes from: its operands, or the instruction itself.
enum class uniformity_source : u8
{
    /// Varying where an operand is.
    operands,
    /// Varying whatever its operands: a thread's id, a stage input, an atomic's result, a local variable's load.
    varying,
    /// The same across the wave whatever its operands: the first lane's value, a ballot, a workgroup's id.
    uniform,
};

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
    uniformity_source source = uniformity_source::operands;
    /// What it spends, per lane of its result: `cost` at `cost_factor`, and `extra` at `extra_factor`.
    /// Unset, `cost` is its kind's own class and the factors one and none.
    bool has_cost = false;
    cost_class cost = cost_class::free;
    f32 cost_factor = 1;
    cost_class extra = cost_class::free;
    f32 extra_factor = 0;
};

/// The class an operation of `kind` spends in, where its format says nothing finer.
[[nodiscard]] cost_class class_of(op_kind kind);

struct graph_block
{
    u32 first = 0;
    u32 end = 0;
    cc::vector<i32> successors;
    /// The value a conditional branch or a switch ending it decides on, or -1.
    i32 condition = -1;
    /// A loop header's trips, where its counter states them; 0 where unknown.
    f64 trips = 0;
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

/// Each block's immediate dominator, block 0 being its own and a block no walk reaches -1.
[[nodiscard]] cc::vector<i32> dominators(graph const& g);

/// A natural loop: its header, and its body, the header and every block that reaches a latch without passing it.
struct natural_loop
{
    i32 header = -1;
    /// Parallel to the blocks.
    cc::vector<bool> body;
};

/// Every natural loop of `g`, one per header, however many latches close it.
[[nodiscard]] cc::vector<natural_loop> natural_loops(graph const& g);

/// Each block's immediate post-dominator, -1 where it is the function's exit or none.
[[nodiscard]] cc::vector<i32> post_dominators(graph const& g);

/// How often each block runs against the entry's once, a wave's way through it.
/// A block runs as often as its immediate dominator, but for an arm of a branch: the wave takes one way of a branch it
/// shares, its share of them, and every way of one it does not, each in full.
/// An arm that dominates a loop's header is that loop's guard, and runs in full too, as does the way a loop's exit test
/// stays in the loop.
/// A natural loop's body runs as often again as its header's `trips`, or `loop_trips` where those are unknown.
[[nodiscard]] cc::vector<f64> frequencies(graph const& g, cc::span<bool const> varying, f64 loop_trips);

/// The edges that go back to a block dominating their source: one per loop's latch, in a format that states no merges.
[[nodiscard]] i32 back_edges(graph const& g);

/// Which values may differ between the threads of a wave: from the varying sources, through every operand, and into a
/// phi that joins the ways a varying branch parted at its immediate dominator.
[[nodiscard]] cc::vector<bool> varying_values(graph const& g);

/// `f` added into `total`: counts summed, the live words and the critical path the worst.
void accumulate(function_metrics const& f, function_metrics& total);
} // namespace shan::impl
