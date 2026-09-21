#pragma once

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// Switches that exist for the tests of the legalizer, each one breaking a rule on purpose.
/// A differential test that still passes with one of them set has no teeth.
struct sgl::check::legalize_options
{
    /// Skips the pins of rule E2, so an operand may be evaluated after the block to its right.
    bool skip_pinning = false;
    /// Skips the tests of rule X5, so an exit that crossed a construct ends there.
    bool skip_flag_tests = false;
};

namespace sgl::check
{
/// The structured form of `e` as a core tree that behaves the same: same result, same prints in the same order.
///
/// Expressions first, cheapest rule first:
/// - E1: a block expression moves in front of the statement that holds it, and its value is a `var` its leaves assign.
///   A block whose only leave is its last statement needs no `var`: its statements move, and its value stays in place.
/// - E2: before that, every operand to its LEFT is pinned into a `let`, unless it has no effect and the block cannot change it.
/// - E3: `and` / `or` whose right operand moved statements or has an effect is an `if` over a `var`.
/// - E4: a `while` whose condition moved statements is a `loop` that starts with `if not c { leave }`.
/// - The `end` of a `for` is pinned unless it is stable, since a target evaluates it before every iteration.
///
/// Then exits:
/// - X1: `leave $root` is `return`.
/// - X2: a block whose leaves all stand in tail position disappears, and what follows a leaving `if` moves into its `else`.
/// - X3, X4: any other block is a `once`, and a leave of the innermost `once` or loop is `break`.
/// - X5: a leave or a continue that crosses a `once` or a loop sets a flag and breaks; the flag is tested after each crossed construct.
///
/// Every name it introduces comes from the entry point's mint.
/// A tree that is core already comes back unchanged, and a `once` or a `break` of the input is read as a block and a leave.
/// Total: a malformed tree gives a tree, which `find_core_violation` may still refuse.
/// `m` must be the module `e` stands in, and it must declare `bool` for a flag to have a type.
[[nodiscard]] flat_entry_point legalize(checked_module const& m,
                                        flat_entry_point const& e,
                                        legalize_options const& options = {});
} // namespace sgl::check
