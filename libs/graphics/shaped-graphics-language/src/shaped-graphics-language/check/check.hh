#pragma once

#include <clean-core/container/span.hh>
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
/// Name resolution, type checking and evaluation of one unnamed module: the files of `prelude` in front, then `user`.
///
/// They stay separate files, so every span keeps pointing into its own source.
/// A file is named by position in that order: prelude file i is file i, and the user file is file `prelude.size()`, the last one.
/// `sgl::prelude_files()` is the library's own prelude, and a test may bring any other, an empty one included.
///
/// A `@builtin` declaration stands for the record of `builtins` that has its name and, for a function, its parameter types.
/// `builtins` must outlive the module, which keeps a pointer to it.
///
/// Total: any ASTs give a module and a list of diagnostics, `invalid` nodes and earlier diagnostics included.
/// What did not check has the error type, and the error type never causes a second diagnostic.
///
/// A tracer: it carries exactly what `tests/samples/` needs, and everything else is `unsupported-yet`.
[[nodiscard]] checked_module check(cc::span<module_file const> prelude,
                                   module_file user,
                                   builtins::registry const& builtins);

/// The same against `builtins::default_registry()`.
[[nodiscard]] checked_module check(cc::span<module_file const> prelude, module_file user);
} // namespace sgl::check
