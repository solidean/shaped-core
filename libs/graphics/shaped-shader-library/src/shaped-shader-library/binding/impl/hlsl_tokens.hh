#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// Where in the shader something is, as the author would recognise it.
///
/// The pass reads a source that has already been flattened, so a raw offset into it names nothing a reader could find.
/// `#line` is what maps it back, and the lexer follows those directives rather than counting lines.
/// `file` is empty for a source carrying no `#line` at all — a snippet, or a single file compiled on its own.
struct hlsl_location
{
    cc::string_view file;
    i32 line = 1;
};

/// "shared.hlsli:12", or "line 12" where the source named no file.
[[nodiscard]] cc::string to_string(hlsl_location const& location);

/// What a token is, at the resolution the binding pass needs — nothing here evaluates an HLSL expression.
enum class hlsl_token_kind
{
    identifier,  ///< a keyword or a name; which one it is, the parser decides by comparing text
    number,      ///< a numeric literal, kept verbatim
    punctuation, ///< exactly one character, so `::` and `>>` arrive as two tokens
    annotation,  ///< a `#pragma sc` line — the pass's own attribute, and the only directive the parser sees
};

/// One token, addressed back into the source, because the rewriter edits around tokens rather than rebuilding text.
struct hlsl_token
{
    hlsl_token_kind kind = hlsl_token_kind::identifier;

    /// The token's own bytes, into the lexed source.
    /// For an annotation this is what follows `#pragma sc`, trimmed.
    cc::string_view text;

    isize offset = 0; ///< where the token starts in the source; for an annotation, at its `#`
    isize length = 0; ///< how much source the token covers, which for an annotation is the whole directive
    hlsl_location location;
};

/// Splits HLSL into tokens, dropping whitespace, comments and the insides of literals.
///
/// Two directives are understood and everything else beginning with `#` is left to lex as ordinary tokens:
/// a `#pragma sc` line becomes an `annotation`, and a `#line` directive moves the location the tokens after it
/// report without becoming a token itself.
/// A pragma is what carries an attribute because DXC's include flatten erases comments and keeps pragmas verbatim —
/// the spike's Q11 and Q12 pin both halves.
///
/// Fails only on an unterminated block comment or literal — everything else is a token to somebody.
[[nodiscard]] cc::result<cc::vector<hlsl_token>> lex_hlsl(cc::string_view hlsl);

/// One argument of an annotation: `key=value`, `key=(a, b, c)`, or a bare token carrying no key.
struct annotation_argument
{
    cc::string_view key;                ///< empty for a positional argument, such as the number in `group 0`
    cc::vector<cc::string_view> values; ///< one element unless the value was a parenthesised tuple
};

/// A parsed `#pragma sc <name> [key=value]...` attribute.
/// Values are `sg` enumerator names spelled exactly, so nothing here translates a vocabulary.
struct annotation
{
    cc::string_view name;
    cc::vector<annotation_argument> arguments;
    hlsl_location location;
};

/// Reads one annotation token's text as the grammar above.
/// `location` is only carried through, for the error the caller reports.
/// Rejects quotes, nesting and anything that would need an expression parser.
[[nodiscard]] cc::result<annotation> parse_annotation(cc::string_view text, hlsl_location const& location);
} // namespace slib::impl
