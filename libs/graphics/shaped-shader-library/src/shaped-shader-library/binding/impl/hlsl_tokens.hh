#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// What a token is, at the resolution the binding pass needs — nothing here evaluates an HLSL expression.
enum class hlsl_token_kind
{
    identifier,  ///< a keyword or a name; which one it is, the parser decides by comparing text
    number,      ///< a numeric literal, kept verbatim
    punctuation, ///< exactly one character, so `::` and `>>` arrive as two tokens
    annotation,  ///< a `//!>` comment — the one comment the parser sees
};

/// One token, addressed back into the source, because the rewriter edits around tokens rather than rebuilding text.
struct hlsl_token
{
    hlsl_token_kind kind = hlsl_token_kind::identifier;

    /// The token's own bytes, into the lexed source.
    /// For an annotation this is what follows `//!>`, trimmed — not the marker and not the `//`.
    cc::string_view text;

    isize offset = 0; ///< where the token starts in the source; for an annotation, at its `//`
    isize length = 0; ///< how much source the token covers, which for an annotation is more than `text.size()`
    i32 line = 1;     ///< 1-based

    /// Whether nothing but whitespace precedes this token on its line.
    /// This is what tells a trailing attribute from one standing on its own line, which is the whole attachment rule.
    bool first_on_line = false;
};

/// Splits HLSL into tokens, dropping whitespace, ordinary comments and the insides of literals.
/// A `//!>` comment survives as an `annotation`, which is what makes an attribute reachable to a parser that
/// otherwise never sees a comment.
/// Fails only on an unterminated block comment or literal — everything else is a token to somebody.
[[nodiscard]] cc::result<cc::vector<hlsl_token>> lex_hlsl(cc::string_view hlsl);

/// One argument of an annotation: `key=value`, `key=(a, b, c)`, or a bare token carrying no key.
struct annotation_argument
{
    cc::string_view key;                ///< empty for a positional argument, such as the number in `group 0`
    cc::vector<cc::string_view> values; ///< one element unless the value was a parenthesised tuple
};

/// A parsed `//!> <name> [key=value]...` attribute.
/// Values are `sg` enumerator names spelled exactly, so nothing here translates a vocabulary.
struct annotation
{
    cc::string_view name;
    cc::vector<annotation_argument> arguments;
    i32 line = 1;
};

/// Reads one annotation token's text as the grammar above.
/// `line` is only carried through, for the error the caller reports.
/// Rejects quotes, nesting and anything that would need an expression parser.
[[nodiscard]] cc::result<annotation> parse_annotation(cc::string_view text, i32 line);
} // namespace slib::impl
