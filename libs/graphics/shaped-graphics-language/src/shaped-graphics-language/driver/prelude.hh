#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

/// One file of the prelude, as the compiler places it in front of every program.
struct sgl::prelude_file
{
    /// What the file is called in a formatted diagnostic: `builtins.sgl`.
    cc::string_view name;
    cc::string_view source;
};

/// One file of the prelude, parsed and built into its AST.
struct sgl::parsed_prelude_file
{
    parsed_file file;
    ast::file_ast ast;
};

namespace sgl
{
/// The prelude, in the order its files stand in a module: `builtins.sgl`, `core.sgl`, `raytracing.sgl`, then `slug.sgl`.
///
/// `builtins.sgl` is `builtins::default_registry().prelude_text()`: generated in memory, so the library never opens the committed file.
/// The committed `prelude/builtins.sgl` is byte-identical, which is why a diagnostic's line and column are right in it.
/// `core.sgl` is the hand-written `prelude/core.sgl` as it was when the library was built.
/// Embedded or generated, so compiling SGL needs no file: a page has no filesystem, and a shipped binary has no source tree.
///
/// The views live for the life of the process.
[[nodiscard]] cc::span<prelude_file const> prelude_files();

/// `prelude_files()` parsed and built, in the same order, once per process rather than once per compile.
/// Immutable after the first call, so every thread reads the same files.
[[nodiscard]] cc::span<parsed_prelude_file const> parsed_prelude();

/// `parsed_prelude()` checked against `builtins::default_registry()`, once per process, which every compile behind it continues.
/// Null where the prelude reports anything, and a compile then checks the prelude with its program to report it.
/// Immutable after the first call, like `parsed_prelude()`.
[[nodiscard]] check::checked_prelude const* checked_prelude();

/// The position in `prelude_files()` of the file `path` is, -1 when it is none of them.
/// Only the library's own prelude directory counts, as the build saw it: a user's `shaders/prelude/core.sgl` is -1.
/// So `path` must be absolute, a path or a `file://` uri; either separator matches, and on Windows any case.
/// A driver checks such a source as that file of the prelude, in place of the library's copy.
/// Behind the prelude it would declare everything a second time, and each name would then hide its own twin.
[[nodiscard]] i32 prelude_file_of(cc::string_view path);

/// Whether `a` and `b` are one path, each a path or a `file://` uri: either separator matches, `.`, `..` and doubled
/// separators are read lexically, and on Windows any case matches.
/// Nothing is opened, so a relative path and its absolute spelling, or a path and a symbolic link to it, are two paths.
[[nodiscard]] bool is_same_path(cc::string_view a, cc::string_view b);
} // namespace sgl

namespace sgl::impl
{
/// Both defined by the file CMake generates from `prelude/core.sgl`.
[[nodiscard]] cc::string_view embedded_core_prelude();
/// `prelude/raytracing.sgl`: the types and the functions of a trace, and the software traversal webgpu runs.
[[nodiscard]] cc::string_view embedded_raytracing_prelude();
/// `prelude/slug.sgl`: Slug's coverage of a shape bounded by quadratic curves, which any pixel shader may call.
[[nodiscard]] cc::string_view embedded_slug_prelude();
/// The absolute path of the source tree's `prelude/` directory, with `/` separators.
[[nodiscard]] cc::string_view prelude_directory();
} // namespace sgl::impl
