#pragma once

#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl
{
/// Tokenizes every line of a file whose line tree is built, and settles each non-blank line's kind.
///
/// A line is tokenized in the mode its parent hands down — code, comment or string content — and hands one down itself.
/// A line that is only a comment makes every line below it a comment.
/// A line ending in an open quote makes every line below it string content, and makes its next sibling start with
/// the closing quote.
/// That is the whole reach of a line: its children and the first token of its next sibling, never a parent and
/// never anything further on.
///
/// Must be called once, on a file that `build_line_tree` produced and nothing has tokenized yet.
void tokenize(parsed_file& file);

/// Letters, digits, `_`, `@`, `#`, `\` and every byte of a non-ASCII code point.
[[nodiscard]] bool is_symbol_start(char c);

/// Letters, digits, `_` and non-ASCII bytes.
/// `-` is not among them, so `a-b` is a subtraction and every name stays spellable in a host language.
[[nodiscard]] bool is_word_char(char c);

/// One of `!+-*/%=<>?&^|~`.
[[nodiscard]] bool is_operator_char(char c);
} // namespace sgl
