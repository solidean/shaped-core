#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/library_file.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

/// A library's module files parsed once for the process, shared by every compile that hands in the same files.
/// It keeps the checked layers of its modules too, each made on first need, so a module is checked once per process.
/// Immutable but for `checked`, which any thread may fill.
struct sgl::shared_library
{
    /// One file parsed, shared by every library that holds it with the same name, source and place.
    struct parsed_unit
    {
        parsed_file file;
        ast::file_ast ast;
    };
    /// One layer of the library's chain of modules, and what it reads: the links before it, and its own files.
    struct chain_link
    {
        /// The module the link adds; empty for a layer of the modules a program reaches, which is no chain's.
        cc::string module;
        cc::shared_ptr<check::checked_prelude const> layer;
        cc::shared_ptr<chain_link const> before;
        cc::vector<cc::shared_ptr<parsed_unit const>> units;
    };
    using link_ptr = cc::shared_ptr<chain_link const>;

    cc::vector<cc::shared_ptr<parsed_unit const>> units;
    /// Each file as the check pass takes it, and the name it was handed in under.
    cc::vector<check::module_file> files;
    cc::vector<cc::string> names;
    /// The files whose sources differ from those of the library made before it under the same names, or that one's
    /// where none does: what is being edited, whose modules its chain puts as late as the modules using them allow.
    /// So every keystroke in a module after the first checks it and the modules using it again, and nothing else.
    cc::vector<i32> late_files;
    /// The library made before it under the same names, whose chain an edit's links are relocated from.
    cc::weak_ptr<shared_library const> previous;

    struct layers
    {
        /// Every module of the library checked, the last link of its chain (`check::library_chain`), which every
        /// compile continues from.
        /// A link is shared with every library that holds the same modules before it, so an edit of a module checks
        /// it and the modules after it again, and none before.
        /// Null once it could not be made, a module that reports anything among the reasons; `made` serves then.
        link_ptr library;
        bool has_tried_library = false;
        /// The chains of the permutations compiled most recently, each with the values a compile gave its modules'
        /// options; the links before the first module it sets are `library`'s.
        cc::vector<link_ptr> with_options;
        /// One layer per set of modules a program reaches, where the library makes none of its own, as a link of no
        /// chain.
        cc::vector<link_ptr> made;
        /// Set once a set of this library's modules failed to make a layer, after which its compiles check it whole.
        bool has_failed = false;
    };
    mutable cc::mutex<layers> checked;
};

/// One module's link of a library's chain as bytes (`check::save_link_artifact`), under its `module_artifact_keys` key.
struct sgl::module_artifact
{
    cc::string key;
    cc::vector<cc::byte> bytes;
};

namespace sgl
{
/// The files of `library` a compile of `source_name` checks, parsed once for the process; null where there are none.
/// The file named like the source (`is_same_path`) is left out, and so is one whose first line plainly declares no module.
/// The key is every name and source handed in, so an edit of any file is another library.
/// A module SGL ships (`shipped_library()`) joins the files where `source` or one of them has a line `use` of it.
/// A few are kept, and every one goes once there are many, so hot reloading or editing cannot grow them without bound.
/// Safe from any thread.
[[nodiscard]] cc::shared_ptr<shared_library const> shared_library_of(cc::span<library_file const> library,
                                                                     cc::string_view source_name,
                                                                     cc::string_view source = {});

/// The link `program` continues from behind `checked_prelude()`: the whole library's chain, or else a layer of the
/// modules it reaches, made on first need and kept in `library`.
/// A compile that sets a module's option continues from a chain checked with its values, which shares every link
/// before that module's with the others; a layer of the modules a program reaches serves the defaults alone.
/// Null where none serves: no checked prelude, a program reaching no module or its own module's files, a failed layer.
/// Hold the link as long as anything checked behind its layer: it keeps the files of the links before it alive.
[[nodiscard]] shared_library::link_ptr layer_of(shared_library const& library,
                                                check::module_file program,
                                                cc::span<check::option_value const> options = {});

/// The keys of the artifacts of the modules of `library`, as `shared_library_of(library, source_name, source)` reads it: what
/// a host that keeps artifacts fetches before a compile, from wherever it keeps them, and hands in.
/// A key names the compiler's build, the module, and its files' sources, and nothing of the modules it uses: an
/// artifact loads over them as they stand, or is a miss.
[[nodiscard]] cc::vector<cc::string> module_artifact_keys(cc::span<library_file const> library,
                                                          cc::string_view source_name,
                                                          cc::string_view source = {});

/// Whether the process makes artifacts of the modules it checks, for a host to keep; off by default, and turning it
/// off drops every artifact handed in or made.
void keep_module_artifacts(bool is_on);

/// Hands the process artifacts a host kept, each under its key: a compile then loads a module's link from its
/// artifact rather than checking it, where it loads over the modules before it as they stand.
/// A compile reads nothing else, so a host that keeps them fetches them first, and asynchronously.
/// A few hundred are kept, and every one goes once there are more.
void offer_module_artifacts(cc::span<module_artifact const> artifacts);

/// The artifacts made since the last call, while `keep_module_artifacts` is on: one per module a compile checked
/// rather than loaded or moved, at its options' defaults; a host keeps them where it will fetch them from.
[[nodiscard]] cc::vector<module_artifact> take_module_artifacts();
} // namespace sgl
