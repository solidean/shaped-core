#pragma once

#include <shaped-graphics-language/fwd.hh>

/// What runs on a program between `construct_ssa` and a writer, each pass in its own right.
///
/// Every one takes a program in SSA form and leaves one, and each operation that stays keeps its statement and its
/// guarantee, so a line of a disassembly names the `.sgl` line it came from, and a guarantee narrower than an entry point
/// holds through all of them.
/// An operation one of them makes in place of others says where in a pass's own contract its statement comes from.
namespace sgl::ir
{
/// Drops every operation nothing reads and whose running has no effect, phis that only read each other too.
void remove_unused(program& p);

/// Replaces an operation with an earlier one of the same operands and guarantee that dominates it.
///
/// What stays keeps its own statement: a line asked for that value second names the line that asked first.
/// Reads of memory a shader cannot write share too, so a value read twice from a binding is one read.
/// Leaves the replaced operation unread, for `remove_unused`, and keeps control flow as it stood.
void share_common_values(program& p);

/// Joins blocks, threads edges past blocks that only pass values on, and turns a branch with little on its sides into
/// `select`s, which run both sides whichever way it would have gone.
///
/// A `select` stands for a phi and keeps that phi's statement and guarantee.
/// Clears every merge and continue target and renumbers the blocks in reverse postorder, dropping what no walk reaches,
/// so this is for a target with no structured control flow to keep, DXIL.
void flatten_branches(program& p);

/// Reorders each run of `fast` additions in one block, so a total meets each product alone and fuses with it.
///
/// A sum only reorders with sums of its own guarantee, and each addition keeps its statement and its place in the block.
/// What is read twice stays a value of its own, so nothing is computed twice.
void chain_sums(program& p);
} // namespace sgl::ir
