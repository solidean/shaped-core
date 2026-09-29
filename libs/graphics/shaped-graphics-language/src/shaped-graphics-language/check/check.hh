#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
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

/// The files of `prelude` checked once, for any number of programs behind them; none where they report anything.
/// A failed prelude is never kept, so a caller falls back to `check` over the files, which reports it.
/// The files and `builtins` must outlive the result.
[[nodiscard]] cc::optional<checked_prelude> check_prelude(cc::span<module_file const> prelude,
                                                          builtins::registry const& builtins);

/// `check` over the prelude `prelude` was made from and `user`, starting from what checking the prelude left.
/// What it gives is what `check` gives, except that every id the program's check makes comes after the prelude's.
/// It reads `prelude` and never writes it, so any number of threads may check against one at once.
[[nodiscard]] checked_module check(checked_prelude const& prelude, module_file user);
} // namespace sgl::check

namespace sgl::check::impl
{
struct checker;
/// How far a check got: the files it declared, and the length of each list a later step walks only past what it held.
struct resume_point
{
    /// The files declared already; 0 is a check from scratch.
    i32 files = 0;
    isize symbols = 0;
    isize extensions = 0;
    isize tests = 0;
    isize wide_literals = 0;
};
} // namespace sgl::check::impl

/// The whole state of the check pass after the prelude alone, which a program's check copies and continues.
struct sgl::check::checked_prelude
{
    checked_prelude(checked_prelude&&) noexcept;
    checked_prelude& operator=(checked_prelude&&) noexcept;
    ~checked_prelude();

private:
    checked_prelude();

    cc::vector<module_file> _files;
    cc::unique_ptr<impl::checker> _state;
    /// How much of each list the prelude filled, which a program's check starts behind.
    impl::resume_point _resume;

    friend cc::optional<checked_prelude> check_prelude(cc::span<module_file const> prelude,
                                                       builtins::registry const& builtins);
    friend checked_module check(checked_prelude const& prelude, module_file user);
};
