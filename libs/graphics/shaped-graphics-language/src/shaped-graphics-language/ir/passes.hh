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
/// A store into a local nothing reads has none.
void remove_unused(program& p);

/// Replaces an operation with an earlier one of the same operands and guarantee that dominates it.
///
/// What stays keeps its own statement: a line asked for that value second names the line that asked first.
/// Reads of memory a shader cannot write share too, so a value read twice from a binding is one read.
/// So does a read of a local after which no store to it can run, and a local whose one store copies such a read of
/// another local reads as that other local, which leaves the copy to `remove_unused`.
/// Leaves the replaced operation unread, for `remove_unused`, and keeps control flow as it stood.
void share_common_values(program& p);

/// Computes once a value several blocks compute alike, none of which dominates another, in their common dominator,
/// where every operand of it already stands.
/// It moves two kinds of value, each of which computing on a path that does not need it changes nothing:
/// - An access chain, which only names an element.
///   DXIL makes a binding array element's handle where the chain stands, so a texture an if-else over samplers reads in
///   each arm is one handle, as DXC makes it, rather than one an arm.
/// - A value the whole dispatch shares, fed only by constants, globals and the constant blocks' elements, which costs a
///   uniform register.
///   An unrolled loop leaves one copy of a test of a constant in every arm of every iteration, which DXC hoists out of
///   the loop before unrolling it.
/// The hoisted value keeps the first twin's statement; run it after `share_common_values`, which leaves no twin dominated.
void hoist_common_values(program& p);

/// Joins blocks, threads edges past blocks that only pass values on, and turns a branch with little on its sides into
/// `select`s, which run both sides whichever way it would have gone.
///
/// A `select` stands for a phi and keeps that phi's statement and guarantee.
/// `unstructured` clears every merge and continue target and renumbers the blocks in reverse postorder, dropping what
/// no walk reaches: that is DXIL's.
/// `structured` keeps every construct it does not fold and every block in the order it opened, what no walk reaches
/// too, so SPIR-V's merge and continue targets still hold.
/// It folds only a selection whose arms head nothing, joins a block only into one no construct names, and threads no
/// edge; a phi of a pointer or a handle keeps its branch, since logical addressing selects neither.
void flatten_branches(program& p, control_flow flow);

/// `flatten_branches` for DXIL, `unstructured`.
void flatten_branches(program& p);

/// Gathers the phis a `construct` reads lane by lane back into one phi of the vector, where each way in hands over the
/// lanes of one vector, the phi itself, or constants, which become a construct ahead of that way's branch.
/// A way whose lanes are the phis of another merge makes those a group first, so nested merges regroup as one.
///
/// SSA construction promotes a vector scalar by scalar, which DXIL wants and SPIR-V does not.
/// A lane's phi becomes an `extract` of the new phi, and keeps its statement and guarantee there.
/// Leaves the lanes' phis unread for `remove_unused`, and keeps control flow as it stood.
void regroup_lanes(program& p);

/// Makes each local written once, whole, with constants, ahead of every read and never again, a `storage::table`.
///
/// That is a `let` of constants an index reaches at run time, which SSA leaves in memory.
/// A target states a table once, where a local is memory each invocation fills first; tables of the same leaves are one.
/// The local's store goes, and so does the local.
void state_constant_tables(program& p);

/// Computes again, after each call point a value would be kept across, every value that reads nothing an invocation
/// or a called shader can change: constant blocks, the launch's index and size, constants, and the arithmetic, addresses
/// and composites made of them alone (EMIT-292).
///
/// A use the walk from its value to it can pass a call point at reads a copy of the value's whole chain, made at the
/// start of the use's stretch of its block between two call points, after its phis; a phi's use is at the end of the
/// way it comes in by.
/// One copy serves every use of its stretch, and a copy keeps its value's statement and guarantee.
/// A debug value that names the value is stated again with the copy, after it, where its variable holds the value on every
/// way from that debug value to the copy (EMIT-267).
/// A read of memory the shader or a called shader can write, a payload, a phi and a resource handle are kept as they are.
/// Run it after every pass that shares or moves values, then `remove_unused` for what nothing reads any more.
void recompute_after_calls(program& p);

/// Reorders each run of `fast` additions in one block, so a total meets each product alone and fuses with it.
///
/// A sum only reorders with sums of its own guarantee, and each addition keeps its statement and its place in the block.
/// What is read twice stays a value of its own, so nothing is computed twice.
void chain_sums(program& p);
} // namespace sgl::ir
