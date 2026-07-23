#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// The lexical category of a token. Punctuators/operators all share `punctuation` and are told apart
/// by `token::text` (via `is_punct`). Trivia (whitespace/newlines/comments) is kept in the stream so
/// spans stay gap-free, but the parser skips it.
enum class token_kind : u8
{
    end_of_file,
    unknown, // a byte we could not classify — best-effort, never fatal

    identifier,
    keyword, // a recognized keyword spelling; every other identifier stays `identifier`

    integer_literal,  // 0, 42, 0x1f, 0b10, 1'000, 100u, 1ll, 0z
    floating_literal, // 1.0, .5, 1e9, 1.5f, 0x1p4
    char_literal,     // 'a', '\n', u'x', '\''
    string_literal,   // "…", raw R"d(…)d", prefixes u8/L/u/U, suffixes sv/s

    punctuation, // every operator/punctuator; disambiguated by `text`

    line_comment,  // // … (honors backslash-newline continuation)
    block_comment, // /* … */
    whitespace,    // a run of spaces/tabs
    newline,       // one logical line break

    preprocessor_directive, // a whole '#' line to end-of-line, kept OPAQUE (not expanded in v1)
};

/// A single lexed token. Carries its source_span and a non-owning view of its spelling.
struct token
{
    token_kind kind = token_kind::unknown;
    source_span span;
    cc::string_view text; // view into source_buffer::text(); not owned

    /// 0 == spelled directly in source (one token, one contiguous range).
    /// RESERVED hook: a future macro-expansion table will map non-zero ids to {invocation, definition}
    /// spans; `span` always stays the *spelling* location. Nothing sets this non-zero in v1.
    u32 expansion_id = 0;

    bool is(token_kind k) const { return kind == k; }
    bool is_punct(cc::string_view p) const { return kind == token_kind::punctuation && text == p; }
    bool is_keyword(cc::string_view k) const { return kind == token_kind::keyword && text == k; }

    /// Whitespace, newlines, and comments — everything the parser skips.
    bool is_trivia() const
    {
        return kind == token_kind::whitespace || kind == token_kind::newline || kind == token_kind::line_comment
            || kind == token_kind::block_comment;
    }
};

/// Whether `word` is one of the C++ keywords shaped-linter recognizes (a pragmatic subset — enough for
/// the constructs the parser walks; unknown identifiers just stay identifiers).
bool is_keyword_spelling(cc::string_view word);

/// A short human-readable name for a kind, for test diagnostics and debug dumps.
cc::string_view token_kind_name(token_kind k);
} // namespace scl
