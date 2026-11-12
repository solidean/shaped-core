#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
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

/// One module of a library's chain: its name, and its files as positions in the library, in the library's order.
struct sgl::check::chained_module
{
    cc::string name;
    cc::vector<i32> files;
    /// Whether a file of it declares an `@option`, which a compile setting it checks this link and those after again.
    bool has_options = false;
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
/// The layer is the modules' check with no options given, so a compile that sets an option of one of its modules does
/// not continue from it; one that sets only its own program's options does (`layer_serves_options`).
/// `library` and `prelude` must outlive the result, and a program continues from it with the same `library`.
[[nodiscard]] cc::optional<checked_prelude> check_modules(checked_prelude const& prelude,
                                                          cc::span<module_file const> library,
                                                          module_file program);

/// Every module of `library` checked once behind `prelude`, for every program that reaches any of them: the last layer
/// of the chain `library_chain` orders, each made by `check_next_module`.
/// A program behind it checks as it does beside the modules it reaches alone: a module it does not use is checked, and
/// nothing of the program sees it (`checked_module::is_reached_file`).
/// None where a module, or the plan of them, reports anything; a caller then falls back to `check_modules`.
/// A file's parse or AST diagnostic counts too, which no check reports and only a program reaching the file may.
/// `library` and `prelude` must outlive the result, and a program continues from it with the same `library`.
[[nodiscard]] cc::optional<checked_prelude> check_library(checked_prelude const& prelude,
                                                          cc::span<module_file const> library);

/// The modules of `library` in the order a chain of layers checks them: each after every module it uses.
/// Of those that may come next, one with none of `late_files` first, positions in `library` of the files an editor is
/// changing, so an edit of one checks as few links again as the modules using it allow; then one with no option, for a
/// compile that sets one; then the one more modules reach.
/// Empty where the library makes no chain: no module, a file that does not parse, or a plan that reports.
[[nodiscard]] cc::vector<chained_module> library_chain(cc::span<module_file const> library,
                                                       cc::span<i32 const> late_files = {});

/// `base`, the prelude's layer or one of the chain over it, with `module` of `library` checked over it: the next layer.
/// The layer before must hold every module `module` uses, which `library_chain`'s order does.
/// None where it does not, and where the module reports anything.
/// A program reaching only the modules a layer holds may continue from it (`may_continue_from`).
/// `options` are the values the compile gives `module`'s own options, each named `module.name` (`options_of_module`).
/// A value for anything else, or one its option does not take, is a report, so none: a check beside the library
/// reports it.
/// `library` and `base` must outlive the result.
[[nodiscard]] cc::optional<checked_prelude> check_next_module(checked_prelude const& base,
                                                              cc::span<module_file const> library,
                                                              cc::string_view module,
                                                              cc::span<option_value const> options = {});

/// `link`, checked over `old_base`, as `check_next_module` would make it over `new_base`, without checking it again:
/// the compilation model's early cutoff.
/// The two bases are links of chains of the same modules, with the same files each, in the same order; any of them may
/// have changed, and `link`'s options must be the defaults.
/// None where a fresh check could differ: `link` names what a changed link's interface does not hold, read the body of
/// a changed link's function, or holds an entry a fresh check would have found in a changed link instead.
/// `new_base` and `link`'s own files must outlive the result; `old_base` need not.
[[nodiscard]] cc::optional<checked_prelude> relocate_link(checked_prelude const& link,
                                                          checked_prelude const& old_base,
                                                          checked_prelude const& new_base);

/// Where the states of `a` and `b` first differ, as the artifact's walk writes them: a path of field positions and
/// element indices from the checker down, `.3.0[12].5`; empty where they are the same.
/// For a test and for debugging, which name what a relocation got otherwise than a fresh check.
[[nodiscard]] cc::string layer_difference(checked_prelude const& a, checked_prelude const& b);

/// The values of `options` that name an option of `module`, `module.name`, in the order given.
[[nodiscard]] cc::vector<option_value> options_of_module(cc::span<option_value const> options, cc::string_view module);

/// Whether `program` may continue from `layer`: a layer of its very modules, or one of the chain holding them all.
[[nodiscard]] bool may_continue_from(checked_prelude const& layer,
                                     cc::span<module_file const> library,
                                     module_file program);

/// Whether a compile setting `options` may continue from `layer`: the values they give the options of the layer's
/// modules are exactly those the layer was checked with, none for a layer checked at the defaults.
/// A program's own options change nothing a layer of modules checked, since no module sees its program.
[[nodiscard]] bool layer_serves_options(checked_prelude const& layer, cc::span<option_value const> options);

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
struct layer_maker;
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

    /// Whether `relocate_link` made the layer rather than a check.
    [[nodiscard]] bool is_relocated() const { return _is_relocated; }

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
    /// Whether the layer is one of the chain `check_next_module` makes, which a program reaching only its modules
    /// continues from; the chain's last is `check_library`'s.
    bool _is_library = false;
    /// The values the layer's modules' options were checked with, each link's after the ones before; none at the defaults.
    cc::vector<option_value> _options;
    /// Whether `relocate_link` made the layer, whose part of every list is an earlier check's, moved whole.
    bool _is_relocated = false;
    /// For a layer of the whole library: the names an emitted name must not take, those of the prelude and of the files
    /// a program reaches, made once per set of reached files and shared by every program reaching the same ones.
    cc::unique_ptr<cc::mutex<cc::map<cc::string, cc::shared_ptr<cc::set<cc::string>>>>> _names_by_reach;
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
    friend bool layer_serves_options(checked_prelude const& layer, cc::span<option_value const> options);
    friend cc::optional<checked_prelude> check_library(checked_prelude const& prelude,
                                                       cc::span<module_file const> library);
    friend cc::optional<checked_prelude> check_next_module(checked_prelude const& base,
                                                           cc::span<module_file const> library,
                                                           cc::string_view module,
                                                           cc::span<option_value const> options);
    friend bool may_continue_from(checked_prelude const& layer, cc::span<module_file const> library, module_file program);
    friend cc::string layer_difference(checked_prelude const& a, checked_prelude const& b);
    friend cc::optional<checked_prelude> relocate_link(checked_prelude const& link,
                                                       checked_prelude const& old_base,
                                                       checked_prelude const& new_base);
    friend cc::optional<cc::vector<cc::byte>> save_link_artifact(checked_prelude const& link,
                                                                 checked_prelude const& base,
                                                                 cc::string_view key);
    friend cc::optional<checked_prelude> load_link_artifact(cc::span<cc::byte const> bytes,
                                                            cc::string_view key,
                                                            checked_prelude const& base,
                                                            cc::span<module_file const> library,
                                                            cc::string_view module);
    friend struct impl::layer_maker;
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
};
