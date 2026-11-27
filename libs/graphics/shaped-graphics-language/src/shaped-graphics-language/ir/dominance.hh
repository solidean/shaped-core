#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/fwd.hh>

/// Who dominates whom in a control-flow graph, for any layer that holds one.
///
/// Nodes are numbered in an order where each one's dominators come before it, node 0 being the entry; a block order of
/// this form is one, and so is a writer's that keeps it.
/// `preds` holds each node's predecessors as a run from `first[node]` to `first[node + 1]`; a node no walk reaches has
/// none, and dominates nothing.
class sgl::ir::dominance
{
public:
    dominance(cc::span<u32 const> first, cc::span<u32 const> preds);

    /// Whether every path from the entry to `b` passes through `a`.
    [[nodiscard]] bool dominates(u32 a, u32 b) const { return _entered[a] <= _entered[b] && _left[b] <= _left[a]; }

    /// The closest strict dominator of `node`, the entry's being itself; `node` must be reached by a walk.
    [[nodiscard]] u32 immediate(u32 node) const { return u32(_idom[node]); }

private:
    /// When a walk of the dominator tree entered and left each node: a node dominates what falls inside its interval.
    cc::vector<u32> _entered;
    cc::vector<u32> _left;
    /// -1 for a node no walk reaches.
    cc::vector<i32> _idom;
};

/// Which nodes a walk reaches only through a marked node, for a pass that keeps no value across what a mark stands for.
///
/// `succs` holds each node's successors as a run from `first[node]` to `first[node + 1]`, in any numbering.
/// A walk from a node starts at its end and never enters it again, since entering it again computes anew what it computed.
/// What a node holds before or after its own mark is the caller's to ask; a walk counts a marked node it passes whole.
class sgl::ir::marked_walks
{
public:
    marked_walks(cc::span<u32 const> first, cc::span<u32 const> succs, cc::span<bool const> is_marked);

    /// Whether a walk from the end of `from` reaches the start of `to` having passed a marked node.
    /// Each `from` asked walks the graph once, and its answers for every `to` are kept.
    [[nodiscard]] bool passes_mark(u32 from, u32 to);

private:
    cc::vector<u32> _first;
    cc::vector<u32> _succs;
    cc::vector<bool> _is_marked;
    /// By node: whether it was walked from yet, and then for every node whether a walk reached it past a mark.
    cc::vector<bool> _is_walked;
    cc::vector<cc::vector<bool>> _past_mark;
};
