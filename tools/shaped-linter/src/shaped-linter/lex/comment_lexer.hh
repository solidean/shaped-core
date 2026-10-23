#pragma once

#include <clean-core/error/result.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/lex/token_stream.hh>

namespace scl
{
/// Tokenize a Rust file far enough to find its comments: the same gap-free `token_stream` the C++ lexer produces.
///
/// It separates what a prose rule and `prose apply`'s code check need apart, and nothing more:
/// comments (`//`, `///`, `//!`, nested `/* */`), strings (escaped, byte and raw `r#"…"#`), char literals
/// told apart from lifetimes, identifiers, numbers, and every other byte as one-character punctuation.
/// No keywords and no grammar: no rule reads Rust code.
cc::result<token_stream> lex_rust(source_buffer const& buffer);

/// The same for Lean 4: comments (`--`, nested `/- -/` with the `/--` and `/-!` doc forms), strings
/// (escaped and raw), char literals told apart from a primed identifier (`x'`), identifiers, numbers,
/// and every other byte as punctuation.
cc::result<token_stream> lex_lean(source_buffer const& buffer);

/// The lexer `language` reads with; markdown has none and comes back empty.
cc::result<token_stream> lex_for(source_buffer const& buffer, source_language language);
} // namespace scl
