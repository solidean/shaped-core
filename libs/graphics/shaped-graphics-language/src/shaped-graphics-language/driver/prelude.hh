#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

namespace sgl
{
/// The text of `prelude/prelude.sgl`, as it was when the library was built.
///
/// Embedded, so compiling SGL needs no file: a page has no filesystem, and a shipped binary has no source tree.
/// It is file 0 of every module `compile_to_text` checks.
[[nodiscard]] cc::string_view prelude_source();

/// The name the prelude has in a formatted diagnostic: `prelude.sgl`.
[[nodiscard]] cc::string_view prelude_name();
} // namespace sgl
