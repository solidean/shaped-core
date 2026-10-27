#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/check/flat_builder.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

/// The passes of `legalize`, each reading the tree `out` holds and appending what it rewrites.
/// Old nodes stay where they are, so an id of the input is an id of `out`; what nothing reaches any more is dropped by `compacted`.

namespace sgl::check::impl
{
using stmt_list = cc::vector<flat_stmt_id>;

/// A mutable local of kind `temporary`: a result, a flag.
[[nodiscard]] local_id add_temporary_var(flat_builder& out, cc::string_view desired, type_id type);

/// `not x`, or `y` when `x` is `not y`.
[[nodiscard]] flat_expr_id negated(flat_builder& out, flat_expr_id x);

/// Rule C1: the body of `out.e` with every `case` written as a `switch` or as a chain of `if` over `==`.
/// It runs between two passes of the expression rules.
/// The first takes a `case` out of the block expression that held it, and leaves its patterns alone, since a pattern
/// runs only where it is reached.
/// The second lowers the conditions the chain writes, so a pattern that holds a block is lowered inside the branch
/// that reaches it.
[[nodiscard]] stmt_list lower_cases(flat_builder& out);

/// Rules E1 to E4: the body of `out.e` without a block expression, with `and` / `or` only over operands without effects.
/// Also reads a `once` as a block and a `break` as a leave, so the exit pass meets one kind of exit.
[[nodiscard]] stmt_list lower_expressions(flat_builder& out, legalize_options const& options);

/// Rules X1 to X5 over what `lower_expressions` returned: no block and no leave is left.
[[nodiscard]] stmt_list lower_exits(flat_builder& out, stmt_list const& body, legalize_options const& options);

/// Rule V0, first: no check and no `assert` is left, since no target writes one, and neither is what computed its condition.
[[nodiscard]] stmt_list erase_checks(flat_builder& out, stmt_list const& body);

/// Rule V1: no local, assignment or result of type void is left, and what evaluating one did stays as an `eval`.
[[nodiscard]] stmt_list erase_void(flat_builder& out, stmt_list const& body);

/// Rule X7, over a core body: a `once` whose every `break` can become a branch is its body, in place, and what follows
/// each `break` is written into the side of its `if` that runs on; never into both, so nothing is written twice.
[[nodiscard]] stmt_list dissolve_onces(flat_builder& out, stmt_list const& body);

/// Rules F1, F2, L1 and L2, last, over a core body: what is constant is its literal, written in place, a branch on
/// one is the side it takes, a let whose value repeats a let in scope reads that one instead, and a let nothing reads
/// is gone where its value has no effect and always has a value.
[[nodiscard]] stmt_list simplify_lets(flat_builder& out, stmt_list const& unfolded);

/// `e` with `body` as its body and only the nodes `body` reaches, numbered in the order a walk meets them.
[[nodiscard]] flat_entry_point compacted(checked_module const& m, flat_entry_point const& e, stmt_list const& body);
} // namespace sgl::check::impl
