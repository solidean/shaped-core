#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_span.hh>
#include <shaped-linter/lex/token.hh>

namespace scl
{
/// A soft lexer diagnostic (unterminated string / comment). The lexer recovers and keeps going, so a
/// diagnostic never stops tokenization — it just records that the input was malformed.
struct lex_diagnostic
{
    source_span span;
    cc::string message;
};

/// The full token sequence of one file. It TILES the file gap-free — trivia (whitespace, newlines,
/// comments) is kept, so byte ranges are exact for fix-its and future macro work. The last token is
/// always `end_of_file`.
struct token_stream
{
    cc::vector<token> tokens;
    cc::vector<lex_diagnostic> diagnostics;
    u32 file_id = 0;

    cc::span<token const> all() const { return tokens; }
};

/// A forward, trivia-skipping view over a token span — what the parser drives. `peek`/`current` never
/// return trivia; `raw_index` exposes the position in the full stream so callers can join spans.
struct token_cursor
{
    explicit token_cursor(cc::span<token const> toks) : _toks(toks) { _skip_trivia(); }

    /// The current significant token (never trivia). At the end this is the `end_of_file` token.
    token const& current() const
    {
        CC_ASSERT(_pos < _toks.size(), "cursor past end");
        return _toks[_pos];
    }

    /// The `ahead`-th significant token from the current one (0 == current). Clamps to end_of_file.
    token const& peek(isize ahead = 0) const
    {
        auto i = _pos;
        for (isize n = 0; n < ahead && i < _toks.size(); ++n)
        {
            ++i;
            while (i < _toks.size() && _toks[i].is_trivia())
                ++i;
        }
        return _toks[i < _toks.size() ? i : _toks.size() - 1];
    }

    bool at_end() const { return current().is(token_kind::end_of_file); }

    void advance()
    {
        if (_pos < _toks.size() && !_toks[_pos].is(token_kind::end_of_file))
            ++_pos;
        _skip_trivia();
    }

    /// Index of the current token in the full (trivia-included) stream.
    isize raw_index() const { return _pos; }

private:
    cc::span<token const> _toks;
    isize _pos = 0;

    void _skip_trivia()
    {
        while (_pos < _toks.size() && _toks[_pos].is_trivia())
            ++_pos;
    }
};
} // namespace scl
