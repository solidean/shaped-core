#pragma once

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// What a text does where an operation of the program would have no value: the robustness profile of LEGAL-65.
enum class sgl::check::undefined_guard : sgl::u8
{
    /// Nothing is guarded, and such an operation is the target's undefined behaviour, at no cost.
    none,
    /// The invocation ends instead, before the operation runs, and nothing reports it.
    abort,
    /// The operation and its operands are printed as a failed `assert` is, and the invocation ends as for `abort`.
    /// Without diagnostics there is nothing to print into, and it is `abort`.
    report,
    /// The operation runs on a substitute operand that has a value, so it gives a value the rule names and the
    /// invocation goes on, as WGSL defines its integer division (LEGAL-68).
    define,
};

/// Switches that exist for the tests of the legalizer, each one breaking a rule on purpose.
/// A differential test that still passes with one of them set has no teeth.
struct sgl::check::legalize_options
{
    /// Skips the pins of rule E2, so an operand may be evaluated after the block to its right.
    bool skip_pinning = false;
    /// Skips the tests of rule X5, so an exit that crossed a construct ends there.
    bool skip_flag_tests = false;
    /// Keeps the emulated form of every `flat_by_target` rather than the native one: the target has no native form.
    bool is_emulated = false;
    /// Drops every print with its values, as every check is dropped: for a text compiled without diagnostics (LEGAL-63).
    /// The tree then prints nothing, which is the one way it may differ from the input's behaviour.
    bool erases_prints = false;
    /// Keeps every `assert` as `if (!condition) { print; end }` rather than dropping it, for a text with diagnostics (LEGAL-64).
    bool lowers_asserts = false;
    /// Writes every print as flat statements over `sg_debug` (LEGAL-70), which is what a target compiles.
    /// Left off, a print stays a `flat_print`, which only the interpreter runs.
    bool expands_prints = false;
    /// The files and their names a site's origin and id read (EMIT-183); empty leaves a site without a place.
    cc::span<module_file const> files;
    cc::span<cc::string_view const> names;
    /// LEGAL-65: what an integer division by zero, or an array index outside the array, does in the text.
    undefined_guard guards_undefined = undefined_guard::none;
    /// The target reads a buffer's length, so a guard also bounds a buffer index (LEGAL-66); MSL's buffers carry none.
    bool guards_buffers = true;
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
/// Last, constants and lets, on a tree that is core already too:
/// - F1: a pure builtin's call, a `not`, an `and` or an `or` over literals is its literal, where it is an int, a uint
///   or a bool.
/// - F2: an `if` on a literal is the side it takes, and a `while` on false is nothing.
/// - L1: a let whose value repeats the value of a let in scope reads that let instead, where the value reads something
///   and is the same wherever the invocation evaluates it.
/// - L2: a let nothing reads is gone, where its value has no effect and always has one.
/// - L3: a let of a block's result `var` is that `var`, under the let's name.
///
/// Every name it introduces comes from the entry point's mint.
/// A tree that is core already comes back meaning the same, and a `once` or a `break` of the input is read as a block and a leave.
/// What comes back is compacted: every node is reachable from the body, and every expression has one parent.
/// Total: a malformed tree gives a tree, which `find_core_violation` may still refuse.
/// `m` must be the module `e` stands in, and it must declare `bool` for a flag to have a type.
[[nodiscard]] flat_entry_point legalize(checked_module const& m,
                                        flat_entry_point const& e,
                                        legalize_options const& options = {});
} // namespace sgl::check
