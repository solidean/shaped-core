#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/emit/emit.hh>

namespace sgl::emit
{
/// Every name a program may not use as its own in the text of `t`.
///
/// That is more than the keywords of the target language.
/// It also holds the predeclared types and the functions an emitter writes, since a local named `mul` would hide the one it calls.
/// The two HLSL targets share one list.
[[nodiscard]] cc::span<cc::string_view const> reserved_words(target t);

[[nodiscard]] bool is_reserved(target t, cc::string_view name);
} // namespace sgl::emit
