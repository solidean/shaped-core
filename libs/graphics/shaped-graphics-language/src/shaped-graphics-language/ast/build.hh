#pragma once

#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl::ast
{
/// Reads the form tree of `file` as declarations, statements and expressions.
///
/// Total: any form tree yields an AST, and what the language has no reading for becomes an `invalid` node that keeps
/// its form, with a diagnostic in `file_ast::diagnostics` saying what was expected.
/// A `missing`, `error` or postfix form becomes an `invalid` node WITHOUT a second diagnostic, since the form parser
/// has reported it already.
/// `file` must have been through every syntactic phase, which `sgl::parse` guarantees, and is not modified.
[[nodiscard]] file_ast build(parsed_file const& file);
} // namespace sgl::ast
