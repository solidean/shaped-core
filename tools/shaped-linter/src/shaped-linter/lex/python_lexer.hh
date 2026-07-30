#pragma once

#include <clean-core/error/result.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/token_stream.hh>

namespace scl
{
/// Tokenize one Python file into the same `token_stream` the C++ lexer produces — spans, trivia and the
/// gap-free tiling are identical, so the reporter, `--fix` and every span-based tool work unchanged.
///
/// It lexes and nothing more: no expressions, no statements, no parse tree.
/// The one structural thing it does recognize is **indentation**, as zero-width `indent` / `dedent`
/// tokens at the first content of each logical line — enough to tell a nested block from a top-level one
/// without a grammar.
/// Blank and comment-only lines never change the level, a newline inside brackets or
/// after a `\` does not end a logical line, and a tab advances to the next multiple of 8.
///
/// Effectively infallible: an unterminated string is recovered with a best-effort token plus a
/// `lex_diagnostic`.
cc::result<token_stream> lex_python(source_buffer const& buffer);

/// Whether `word` is a Python keyword (the full list, including the soft `match` / `case` / `type`).
bool is_python_keyword_spelling(cc::string_view word);
} // namespace scl
