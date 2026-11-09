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
        cc::shared_ptr<check::checked_prelude const> layer;
        cc::shared_ptr<chain_link const> before;
        cc::vector<cc::shared_ptr<parsed_unit const>> units;
    };
    using link_ptr = cc::shared_ptr<chain_link const>;

    cc::vector<cc::shared_ptr<parsed_unit const>> units;
    /// Each file as the check pass takes it, and the name it was handed in under.
    cc::vector<check::module_file> files;
    cc::vector<cc::string> names;

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

namespace sgl
{
/// The files of `library` a compile of `source_name` checks, parsed once for the process; null where there are none.
/// The file named like the source (`is_same_path`) is left out, and so is one whose first line plainly declares no module.
/// The key is every name and source handed in, so an edit of any file is another library.
/// A few are kept, and every one goes once there are many, so hot reloading or editing cannot grow them without bound.
/// Safe from any thread.
[[nodiscard]] cc::shared_ptr<shared_library const> shared_library_of(cc::span<library_file const> library,
                                                                     cc::string_view source_name);

/// The link `program` continues from behind `checked_prelude()`: the whole library's chain, or else a layer of the
/// modules it reaches, made on first need and kept in `library`.
/// A compile that sets a module's option continues from a chain checked with its values, which shares every link
/// before that module's with the others; a layer of the modules a program reaches serves the defaults alone.
/// Null where none serves: no checked prelude, a program reaching no module or its own module's files, a failed layer.
/// Hold the link as long as anything checked behind its layer: it keeps the files of the links before it alive.
[[nodiscard]] shared_library::link_ptr layer_of(shared_library const& library,
                                                check::module_file program,
                                                cc::span<check::option_value const> options = {});
} // namespace sgl
