#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// One file of the prelude, as the compiler places it in front of every program.
struct sgl::prelude_file
{
    /// What the file is called in a formatted diagnostic: `builtins.sgl`.
    cc::string_view name;
    cc::string_view source;
};

namespace sgl
{
/// The prelude, in the order its files stand in a module: `builtins.sgl`, then `core.sgl`.
///
/// `builtins.sgl` is `builtins::default_registry().prelude_text()`: generated in memory, so the library never opens the committed file.
/// The committed `prelude/builtins.sgl` is byte-identical, which is why a diagnostic's line and column are right in it.
/// `core.sgl` is the hand-written `prelude/core.sgl` as it was when the library was built.
/// Embedded or generated, so compiling SGL needs no file: a page has no filesystem, and a shipped binary has no source tree.
///
/// The views live for the life of the process.
[[nodiscard]] cc::span<prelude_file const> prelude_files();

/// The position in `prelude_files()` of the file `path` is, -1 when it is none of them.
/// Only the library's own prelude directory counts, as the build saw it: a user's `shaders/prelude/core.sgl` is -1.
/// So `path` must be absolute, a path or a `file://` uri; either separator matches, and on Windows any case.
/// A driver checks such a source as that file of the prelude, in place of the library's copy.
/// Behind the prelude it would declare everything a second time, and each name would then hide its own twin.
[[nodiscard]] i32 prelude_file_of(cc::string_view path);
} // namespace sgl

namespace sgl::impl
{
/// Both defined by the file CMake generates from `prelude/core.sgl`.
[[nodiscard]] cc::string_view embedded_core_prelude();
/// The absolute path of the source tree's `prelude/` directory, with `/` separators.
[[nodiscard]] cc::string_view prelude_directory();
} // namespace sgl::impl
