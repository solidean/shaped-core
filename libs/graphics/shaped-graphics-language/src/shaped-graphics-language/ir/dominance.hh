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
