#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// Token kinds are deliberately few.
/// There are no keyword, number, attribute or identifier tokens: all of those are `symbol` here and are told apart
/// one phase later, so `10a7` and `@range` and `let` tokenize the same way.
enum class sgl::token_kind : sgl::u8
{
    /// A run of symbol characters; see `is_symbol_start` and `is_word_char`.
    symbol,
    /// The symbol that is exactly `_`.
    wildcard,

    dot,
    comma,
    semicolon,
    colon,
    /// `::`, kept as one token so a diagnostic can name it.
    double_colon,
    /// `->`
    arrow,
    /// `=>`
    double_arrow,
    /// Any other run of operator characters, and any run opening with `..`; its meaning comes from its text.
    op,

    round_open,
    round_close,
    square_open,
    square_close,
    curly_open,
    curly_close,

    /// `"`, `'` or a backquote; which one is the byte it spans.
    quote_open,
    quote_close,
    /// Everything between two quotes on one line, escapes unprocessed; or a whole line of multi-line string content.
    string_body,

    /// `//` to the end of the line, or a whole line owned by a comment-only line above it.
    comment,

    /// Bytes no rule recognizes; consecutive ones form a single token.
    error,
};

namespace sgl
{

[[nodiscard]] cc::string_view to_string(token_kind kind);

} // namespace sgl

/// Whitespace is not a token: it is the gap between two token spans, which is what keeps the source reproducible
/// from the tokens without anything having to store it.
struct sgl::token
{
    token_kind kind;
    source_span where;
};
