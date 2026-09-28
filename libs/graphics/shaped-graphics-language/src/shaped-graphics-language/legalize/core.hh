#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/flat.hh>

/// The definition of the core form: the flat trees every target prints one to one.
///
/// - No `flat_block` and no `flat_leave`: an expression holds no statement, and every exit is `break`, `continue` or `return`.
/// - A `break` stands inside a `once` or a loop, and exits the innermost one.
/// - A `continue` names the innermost loop, and NO `once` stands between the two.
///   A `continue` inside `do { … } while (false)` would end the once, and inside WGSL's `loop { … break; }` it would spin.
/// - The right operand of `and` / `or` has no effect, since not every target promises to skip it.
/// - The `end` of a `for` has no effect and reads no mutable local, since a target evaluates it before every iteration.
///   It takes no derivatives either: after a divergent `break`, that re-test runs in part of a quad.
/// - An assignment whose place has an index with an effect has a value without one, since a target may write the
///   value's effect first.
/// - `return` is legal at any depth.

/// Why a tree is not core: the first offending node in a walk from the body, statements in order.
struct sgl::check::core_violation
{
    /// For a reader and for an emit error's detail: "a leave of $found, which must be a break, a continue or a return".
    cc::string reason;
    /// The statement that offends, or the one that holds the offending expression.
    flat_stmt_id stmt = flat_stmt_id::none;
    flat_expr_id expr = flat_expr_id::none;

    bool operator==(core_violation const&) const = default;
};

namespace sgl::check
{
/// Empty for a core tree.
/// Total: an id that names nothing and a tree nested beyond any real program are violations, never an assertion.
/// `m` is the module the tree's calls are resolved in, which says which of them take derivatives.
[[nodiscard]] cc::optional<core_violation> find_core_violation(checked_module const& m, flat_entry_point const& e);

[[nodiscard]] bool is_core(checked_module const& m, flat_entry_point const& e);

/// True when `id` calls a builtin that takes derivatives, or cannot be walked.
[[nodiscard]] bool takes_derivatives(checked_module const& m, flat_entry_point const& e, flat_expr_id id);

/// True when evaluating `id` does something besides giving its value: a call that is not pure, or a block expression.
/// A block expression counts whatever it holds, since only its statements could say otherwise.
/// A read is no effect, so whether a LATER write changes what `id` gives is a separate question.
/// `id` must name an expression of `e`.
[[nodiscard]] bool has_effect(flat_entry_point const& e, flat_expr_id id);
} // namespace sgl::check
