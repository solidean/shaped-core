#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl::ast
{
/// The AST as s-expressions, made to be read beside the source it came from.
///
/// One top-level declaration per line; a member, a statement, a `case` arm and an `if` branch each get a line of
/// their own, indented two spaces under their owner, and an expression stays on the line of what holds it.
///
///     (fun{@vertex} shade (params (field n : vec3) (field k : float = num:0.5)) (uses frame) -> vec3
///       (let mut c : vec3 = (call:paren lit n light=(member frame sun)))
///       (if
///         (branch (call:infix < k num:0) => (return (tuple ..c num:0)))
///         (else
///           (assign *= c k)))
///       (return c))
///
/// - A node is `(kind …)`; a `name`, `self`, `_` and `.point` are written as in the source, a literal is `num:1`,
///   `str:"…"` or `hash:#fff`.
/// - A call says how it was spelled: `call:paren`, `call:juxt`, `call:infix`, `call:prefix`, and `and` / `or` are
///   `call:infix:short-circuit`; an operator call leads with its operator.
/// - An element of a paren list is its value, `name=value`, `..value` for a splat, or `name=<shorthand>`.
/// - `:` and `->` stand before exactly the expressions in a type position: `(cast x : mat3)`, `(field n : vec3)`,
///   and `(type color : vec4)` for `type color = vec4`.
/// - A body is ` => ` and one expression or statement, or a block of lines.
/// - A lambda says how it was spelled: `(lambda (params …) …)` for `x => …`, `(lambda:fun …)` for an anonymous `fun`,
///   which alone may show `(type-params …)`, `(uses …)` and ` -> type`.
/// - Attributes follow the kind of a declaration, statement or field, and follow a whole expression or element:
///   `(struct{@vertex} v`, `vec4{@c}`.
///   An attribute's arguments are list elements like any other: `{@slider(num:0 max=num:1)}`.
/// - `(invalid "…")`, `(invalid-stmt "…")` and `(invalid-decl "…")` quote the first line of what did not fit,
///   and `<missing>` stands for a name nobody wrote.
///
/// For tests and for reading a tree by eye; the format is not stable and must not be parsed.
[[nodiscard]] cc::string dump(parsed_file const& file, file_ast const& ast);

/// One line per AST diagnostic, in the order they were reported: `stray-else @6+4`.
[[nodiscard]] cc::string dump_diagnostics(file_ast const& ast);
} // namespace sgl::ast
