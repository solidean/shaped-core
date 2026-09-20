#pragma once

#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

/// One file of a module: what `sgl::parse` and `sgl::ast::build` made of it.
/// Both must outlive the call they are passed to, and `ast` must have been built from `file`.
struct sgl::check::module_file
{
    parsed_file const& file;
    ast::file_ast const& ast;
};

namespace sgl::check
{
/// Name resolution, type checking and evaluation of one unnamed module: `prelude` in front, then `user`.
///
/// The two stay separate files, so every span keeps pointing into its own source.
/// A file is named by position: the prelude is file 0 and the user file is file 1.
///
/// Total: any pair of ASTs gives a module and a list of diagnostics, `invalid` nodes and earlier diagnostics included.
/// What did not check has the error type, and the error type never causes a second diagnostic.
///
/// A tracer: it carries exactly what `tests/samples/cube.sgl` needs, and everything else is `unsupported-yet`.
[[nodiscard]] checked_module check(module_file prelude, module_file user);
} // namespace sgl::check
