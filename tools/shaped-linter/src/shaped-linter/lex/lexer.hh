#pragma once

#include <clean-core/error/result.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/token_stream.hh>

namespace scl
{
/// Tokenize one source file. The result tiles the whole file gap-free (trivia included) and ends with
/// an `end_of_file` token. Effectively infallible: malformed input (unterminated string/comment) is
/// recovered with a best-effort token plus a `lex_diagnostic`; the `result` fails only on an impossible
/// internal state. Directives are kept opaque — `#…` lines are one token, not expanded.
cc::result<token_stream> lex(source_buffer const& buffer);
} // namespace scl
