#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{

/// The word naming a directive: `include` for `#include <x>`, `endif` for `#  endif`.
/// The argument is a `preprocessor_directive` token's text — one whole logical line, continuations already
/// folded and any trailing comment already lexed off.
cc::string_view directive_word(cc::string_view text);

/// What an `#include` names, delimiters included: `<atomic>` or `"local.hh"`.
///
/// Empty when the line is not an include, or when the header is spelled by a macro (`#include HEADER`) or
/// left unterminated — neither of which a single-file linter can resolve, so neither is guessed at.
cc::string_view include_target(cc::string_view text);

} // namespace scl
