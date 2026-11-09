#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/features.hh>
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
/// Name resolution, type checking and evaluation of `program`, behind the files of `prelude` and the modules of
/// `library` it reaches through `use`.
///
/// A library file belongs to the module its `module` line names; one without that line is no module's and is left out.
/// Only the modules the program reaches are checked, and `checked_module::library_files` says which files those were.
/// A program whose own `module` line names a module of the library is checked together with that module's files.
///
/// They stay separate files, so every span keeps pointing into its own source.
/// A file is named by position: the prelude's first, then the reached library files, then the program, the last one.
/// `sgl::prelude_files()` is the library's own prelude, and a test may bring any other, an empty one included.
///
/// A `@builtin` declaration stands for the record of `builtins` that has its name and, for a function, its parameter types.
/// `builtins` must outlive the module, which keeps a pointer to it.
///
/// Total: any ASTs give a module and a list of diagnostics, `invalid` nodes and earlier diagnostics included.
/// What did not check has the error type, and the error type never causes a second diagnostic.
///
/// A tracer: it carries exactly what `tests/samples/` needs, and everything else is `unsupported-yet`.
///
/// `options` sets the program's options by name, each standing for its value wherever it is named (CHK-354).
/// A module's option the program uses is named `module.name`.
/// An option `options` leaves out keeps its default, and a name no option has is `invalid-option`.
/// `target` is what the compile writes for, which an inlined body reads through `target_api`, `target_language` and
/// `has` (CHK-426); the default is no target, as a test runs.
[[nodiscard]] checked_module check(cc::span<module_file const> prelude,
                                   cc::span<module_file const> library,
                                   module_file program,
                                   builtins::registry const& builtins,
                                   cc::span<option_value const> options = {},
                                   compile_target const& target = {});

/// The same with no library.
[[nodiscard]] checked_module check(cc::span<module_file const> prelude,
                                   module_file user,
                                   builtins::registry const& builtins,
                                   cc::span<option_value const> options = {},
                                   compile_target const& target = {});

/// The same against `builtins::default_registry()`.
[[nodiscard]] checked_module check(cc::span<module_file const> prelude,
                                   module_file user,
                                   cc::span<option_value const> options = {},
                                   compile_target const& target = {});

/// The files of `prelude` checked once, for any number of programs behind them; none where they report anything.
/// A failed prelude is never kept, so a caller falls back to `check` over the files, which reports it.
/// The files and `builtins` must outlive the result.
[[nodiscard]] cc::optional<checked_prelude> check_prelude(cc::span<module_file const> prelude,
                                                          builtins::registry const& builtins);

/// `check` over the prelude `prelude` was made from, `library` and `program`, starting from what checking the prelude
/// left.
/// What it gives is what `check` gives, except that every id the program's check makes comes after the prelude's.
/// It reads `prelude` and never writes it, so any number of threads may check against one at once.
[[nodiscard]] checked_module check(checked_prelude const& prelude,
                                   cc::span<module_file const> library,
                                   module_file program,
                                   cc::span<option_value const> options = {},
                                   compile_target const& target = {});

/// The same with no library.
[[nodiscard]] checked_module check(checked_prelude const& prelude,
                                   module_file user,
                                   cc::span<option_value const> options = {},
                                   compile_target const& target = {});

/// The prelude and the library modules `program` reaches, checked once as one layer behind `prelude`, for every program
/// that reaches the same ones: the compilation model's checked layers, with the modules a program uses as one layer.
/// None where the module plan or the modules report anything, so a caller falls back to `check` with the library.
/// The layer is the modules' check with no options given; a compile that sets options does not continue from it.
/// `library` and `prelude` must outlive the result, and a program continues from it with the same `library`.
[[nodiscard]] cc::optional<checked_prelude> check_modules(checked_prelude const& prelude,
                                                          cc::span<module_file const> library,
                                                          module_file program);

/// Whether `program` reaches modules of `library` that `check_modules` may make a layer of: some, cleanly, and none of
/// them the program's own module.
[[nodiscard]] bool reaches_modules_to_layer(cc::span<module_file const> library, module_file program);

/// Whether `program` reaches exactly the modules of `library` that `layer` holds, so it may continue from it.
[[nodiscard]] bool reaches_the_same_modules(checked_prelude const& layer,
                                            cc::span<module_file const> library,
                                            module_file program);
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
    /// `notes` and `calls`: a lower layer's functions and calls, whose loops were found when it was checked.
    isize notes = 0;
    isize calls = 0;
    /// `call_records` and `types`: a lower layer's generic calls and open types, instantiated when it was checked.
    isize call_records = 0;
    isize types = 0;
};
} // namespace sgl::check::impl

/// The whole state of the check pass after the prelude alone, which a program's check copies and continues.
struct sgl::check::checked_prelude
{
    checked_prelude(checked_prelude&&) noexcept;
    checked_prelude& operator=(checked_prelude&&) noexcept;
    ~checked_prelude();

    /// The prelude's own tables, the part of every table a program's check behind it starts with.
    [[nodiscard]] checked_module const& module() const;

private:
    checked_prelude();

    /// The prelude's files, then the library files of a layer `check_modules` made, in the order they were checked.
    cc::vector<module_file> _files;
    /// How many of `_files` are the prelude's.
    i32 _prelude_count = 0;
    /// The library files a layer of `check_modules` holds, as positions in its library; empty for the prelude alone.
    cc::vector<i32> _library_order;
    /// The modules of that layer, by name, the program's own first.
    cc::vector<cc::string> _modules;
    cc::unique_ptr<impl::checker> _state;
    /// How much of each list the prelude filled, which a program's check starts behind.
    impl::resume_point _resume;
    /// The text a layer read back from an artifact points into where it is in none of `_files`.
    cc::vector<cc::unique_ptr<cc::string>> _texts;

    friend cc::optional<checked_prelude> check_prelude(cc::span<module_file const> prelude,
                                                       builtins::registry const& builtins);
    friend checked_module check(checked_prelude const& prelude,
                                cc::span<module_file const> library,
                                module_file program,
                                cc::span<option_value const> options,
                                compile_target const& target);
    friend cc::optional<checked_prelude> check_modules(checked_prelude const& prelude,
                                                       cc::span<module_file const> library,
                                                       module_file program);
    friend bool reaches_the_same_modules(checked_prelude const& layer,
                                         cc::span<module_file const> library,
                                         module_file program);
    friend cc::optional<cc::vector<cc::byte>> save_artifact(checked_prelude const& layer, cc::string_view key);
    friend cc::optional<checked_prelude> load_artifact(cc::span<cc::byte const> bytes,
                                                       cc::string_view key,
                                                       cc::span<module_file const> prelude,
                                                       builtins::registry const& builtins);
    friend cc::optional<checked_prelude> load_modules_artifact(cc::span<cc::byte const> bytes,
                                                               cc::string_view key,
                                                               checked_prelude const& prelude,
                                                               cc::span<module_file const> library,
                                                               module_file program,
                                                               builtins::registry const& builtins);
    friend cc::optional<cc::hash128> modules_artifact_id(cc::string_view key,
                                                         checked_prelude const& prelude,
                                                         cc::span<module_file const> library,
                                                         module_file program);
};
