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

/// Rules E1 to E4: the body of `out.e` without a block expression, with `and` / `or` only over operands without effects.
/// Also reads a `once` as a block and a `break` as a leave, so the exit pass meets one kind of exit.
[[nodiscard]] stmt_list lower_expressions(flat_builder& out, legalize_options const& options);

/// Rules X1 to X5 over what `lower_expressions` returned: no block and no leave is left.
[[nodiscard]] stmt_list lower_exits(flat_builder& out, stmt_list const& body, legalize_options const& options);

/// `e` with `body` as its body and only the nodes `body` reaches, numbered in the order a walk meets them.
[[nodiscard]] flat_entry_point compacted(checked_module const& m, flat_entry_point const& e, stmt_list const& body);
} // namespace sgl::check::impl
