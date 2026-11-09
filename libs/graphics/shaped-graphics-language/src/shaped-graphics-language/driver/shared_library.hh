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
    cc::vector<cc::unique_ptr<parsed_file>> parsed;
    cc::vector<cc::unique_ptr<ast::file_ast>> built;
    /// Each file as the check pass takes it, and the name it was handed in under.
    cc::vector<check::module_file> files;
    cc::vector<cc::string> names;

    struct layers
    {
        /// Every module of the library as one layer (`check::check_library`), which every compile continues from.
        /// Null once it could not be made, a module that reports anything among the reasons; `made` serves then.
        cc::shared_ptr<check::checked_prelude const> library;
        bool has_tried_library = false;
        /// One layer per set of modules a program reaches, where the library makes none of its own.
        cc::vector<cc::shared_ptr<check::checked_prelude const>> made;
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

/// The layer `program` continues from behind `checked_prelude()`: the whole library's, or else that of the modules it
/// reaches, made on first need and kept in `library`.
/// Null where none serves: no checked prelude, a program reaching no module or its own module's files, a failed layer.
/// A layer is checked at the modules' defaults, so a compile that sets one of their options asks
/// `check::layer_serves_options` before it continues from it.
[[nodiscard]] cc::shared_ptr<check::checked_prelude const> layer_of(shared_library const& library,
                                                                    check::module_file program);
} // namespace sgl
