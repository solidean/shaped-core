#include "comment_lexer.hh"

#include <clean-core/common/utility.hh>
#include <shaped-linter/lex/lexer.hh>
#include <shaped-linter/lex/python_lexer.hh>

namespace scl
{
namespace
{
bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/// Rust and Lean differ only in their comment delimiters and in what an identifier may hold.
struct comment_syntax
{
    cc::string_view line;         // `//` or `--`
    char block_inner = '*';       // the second byte of a block delimiter: `/*` and `*/`, or `/-` and `-/`
    bool primes_in_names = false; // Lean's `x'` and `h₁'`: an apostrophe after a name continues it
    bool lean_names = false;      // Lean's `!` and `?` may end a name: `get!`, `find?`
};

/// One pass over the file, every token spanning a byte range and the whole file tiled gap-free.
struct comment_scanner
{
    cc::string_view src;
    u32 file_id = 0;
    comment_syntax syntax;
    isize p = 0;
    token_stream out;

    char at(isize k) const { return (p + k) < src.size() ? src[p + k] : '\0'; }
    char cur() const { return at(0); }
    bool eof() const { return p >= src.size(); }
    bool starts(cc::string_view s) const { return src.subview({.start = p, .end = src.size()}).starts_with(s); }

    static bool is_name_start(char c)
    {
        // A UTF-8 lead or continuation byte is let through, so `ε₁` or `α` stays one name.
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || u8(c) >= 0x80;
    }
    bool is_name_cont(char c) const { return is_name_start(c) || is_digit(c) || (syntax.primes_in_names && c == '\''); }

    void emit(token_kind kind, isize start)
    {
        out.tokens.push_back({.kind = kind,
                              .span = {.file_id = file_id, .byte_begin = u32(start), .byte_end = u32(p)},
                              .text = src.subview({.start = start, .end = p})});
    }

    void diag(isize start, cc::string message)
    {
        out.diagnostics.push_back(
            {.span = {.file_id = file_id, .byte_begin = u32(start), .byte_end = u32(p)}, .message = cc::move(message)});
    }

    void run()
    {
        while (!eof())
            next();
        auto const end = u32(src.size());
        out.tokens.push_back({.kind = token_kind::end_of_file,
                              .span = {.file_id = file_id, .byte_begin = end, .byte_end = end},
                              .text = {}});
    }

    void next()
    {
        auto const start = p;
        auto const c = cur();

        if (c == '\n' || c == '\r')
        {
            if (c == '\r' && at(1) == '\n')
                ++p;
            ++p;
            emit(token_kind::newline, start);
            return;
        }
        if (c == ' ' || c == '\t' || c == '\f')
        {
            while (cur() == ' ' || cur() == '\t' || cur() == '\f')
                ++p;
            emit(token_kind::whitespace, start);
            return;
        }
        if (starts(syntax.line))
        {
            while (!eof() && cur() != '\n' && cur() != '\r')
                ++p;
            emit(token_kind::line_comment, start);
            return;
        }
        if (c == '/' && at(1) == syntax.block_inner)
        {
            scan_block_comment(start);
            return;
        }
        if (c == '"')
        {
            scan_string(start);
            return;
        }
        if (c == '\'')
        {
            scan_quote(start);
            return;
        }
        if (is_name_start(c))
        {
            scan_name(start);
            return;
        }
        if (is_digit(c))
        {
            while (is_name_cont(cur()) || (cur() == '.' && is_digit(at(1))))
                ++p;
            emit(token_kind::integer_literal, start);
            return;
        }

        ++p;
        emit(token_kind::punctuation, start);
    }

    /// `/* … */` or `/- … -/`, nested in both languages.
    void scan_block_comment(isize start)
    {
        auto depth = 0;
        while (!eof())
        {
            if (cur() == '/' && at(1) == syntax.block_inner)
            {
                ++depth;
                p += 2;
                continue;
            }
            if (cur() == syntax.block_inner && at(1) == '/')
            {
                p += 2;
                if (--depth == 0)
                {
                    emit(token_kind::block_comment, start);
                    return;
                }
                continue;
            }
            ++p;
        }
        emit(token_kind::block_comment, start);
        diag(start, cc::string("unterminated block comment"));
    }

    /// An escaped string from its opening quote; it may span lines in both languages.
    void scan_string(isize start)
    {
        ++p;
        while (!eof() && cur() != '"')
            p += cur() == '\\' && p + 1 < src.size() ? 2 : 1;
        if (eof())
        {
            emit(token_kind::string_literal, start);
            diag(start, cc::string("unterminated string literal"));
            return;
        }
        ++p;
        emit(token_kind::string_literal, start);
    }

    /// A raw string from its `#` run or opening quote, the `r` (and any `b` / `c`) already consumed: it ends at a quote followed by as many `#`.
    void scan_raw_string(isize start)
    {
        auto hashes = isize(0);
        while (cur() == '#')
        {
            ++hashes;
            ++p;
        }
        ++p; // the opening quote
        while (!eof())
        {
            if (cur() == '"')
            {
                auto run = isize(0);
                while (run < hashes && at(1 + run) == '#')
                    ++run;
                if (run == hashes)
                {
                    p += 1 + hashes;
                    emit(token_kind::string_literal, start);
                    return;
                }
            }
            ++p;
        }
        emit(token_kind::string_literal, start);
        diag(start, cc::string("unterminated raw string literal"));
    }

    /// Whether a raw string opens at `p`: any number of `#` and then a quote.
    bool raw_string_opens() const
    {
        auto k = isize(0);
        while (at(k) == '#')
            ++k;
        return at(k) == '"';
    }

    /// The byte length of the UTF-8 character at `p + k`.
    isize char_length(isize k) const
    {
        auto const lead = u8(at(k));
        return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    }

    /// A char literal, or an apostrophe that is not one: a Rust lifetime or label (`'a`), a Lean prime or quotation.
    void scan_quote(isize start)
    {
        if (at(1) == '\\')
        {
            p += 2;
            while (!eof() && cur() != '\'' && cur() != '\n')
                ++p;
            if (cur() == '\'')
                ++p;
            emit(token_kind::char_literal, start);
            return;
        }
        if (at(1) != '\'' && at(1) != '\n' && at(1 + char_length(1)) == '\'')
        {
            p += 2 + char_length(1);
            emit(token_kind::char_literal, start);
            return;
        }
        ++p;
        emit(token_kind::punctuation, start);
    }

    void scan_name(isize start)
    {
        while (!eof() && is_name_cont(cur()))
            ++p;
        auto const word = src.subview({.start = start, .end = p});

        // A string prefix: `b"…"`, `c"…"`, and the raw forms `r"…"`, `r#"…"#`, `br#"…"#`.
        if (word == "r" || word == "br" || word == "cr")
        {
            if (raw_string_opens())
            {
                scan_raw_string(start);
                return;
            }
        }
        if ((word == "b" || word == "c") && cur() == '"')
        {
            scan_string(start);
            return;
        }
        if (word == "b" && cur() == '\'')
        {
            scan_quote(start);
            return;
        }

        if (syntax.lean_names)
            while (cur() == '!' || cur() == '?')
                ++p;
        emit(token_kind::identifier, start);
    }
};

cc::result<token_stream> lex_with(source_buffer const& buffer, comment_syntax syntax)
{
    comment_scanner s = {.src = buffer.text(), .file_id = buffer.file_id(), .syntax = syntax};
    s.out.file_id = buffer.file_id();
    s.run();
    return cc::move(s.out);
}
} // namespace

cc::result<token_stream> lex_rust(source_buffer const& buffer)
{
    return lex_with(buffer, {.line = "//", .block_inner = '*'});
}

cc::result<token_stream> lex_lean(source_buffer const& buffer)
{
    return lex_with(buffer, {.line = "--", .block_inner = '-', .primes_in_names = true, .lean_names = true});
}

cc::result<token_stream> lex_for(source_buffer const& buffer, source_language language)
{
    switch (language)
    {
    case source_language::cpp:
        return lex(buffer);
    case source_language::python:
        return lex_python(buffer);
    case source_language::rust:
        return lex_rust(buffer);
    case source_language::lean:
        return lex_lean(buffer);
    case source_language::markdown:
        break;
    }
    return token_stream{};
}
} // namespace scl
