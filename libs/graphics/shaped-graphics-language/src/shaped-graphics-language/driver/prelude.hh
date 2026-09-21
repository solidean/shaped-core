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
} // namespace sgl

namespace sgl::impl
{
/// Defined by the file CMake generates from `prelude/core.sgl`.
[[nodiscard]] cc::string_view embedded_core_prelude();
} // namespace sgl::impl
