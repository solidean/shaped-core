#include "lexer.hh"

#include <clean-core/common/utility.hh>

namespace scl
{
namespace
{
bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}
bool is_ident_start(char c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_ident_cont(char c)
{
    return is_ident_start(c) || is_digit(c);
}

/// A non-raw string/char encoding prefix (`L`, `u`, `u8`, `U`), or empty.
bool is_encoding_prefix(cc::string_view s)
{
    return s.empty() || s == "L" || s == "u" || s == "U" || s == "u8";
}

/// A raw-string prefix (ends in `R`): `R`, `LR`, `uR`, `UR`, `u8R`.
bool is_raw_prefix(cc::string_view s)
{
    return s == "R" || s == "LR" || s == "uR" || s == "UR" || s == "u8R";
}

/// The single-pass tokenizer. Cursor over the file text; every token spans a byte range, and the
/// tokens tile the file gap-free (trivia included).
struct scanner
{
    cc::string_view src;
    u32 file_id = 0;
    isize p = 0;
    bool line_has_content = false; // reset on newline; a '#' before any content opens a directive
    token_stream out;

    char at(isize k) const { return (p + k) < src.size() ? src[p + k] : '\0'; }
    char cur() const { return at(0); }
    bool eof() const { return p >= src.size(); }

    source_span span(isize start, isize end) const
    {
        return {.file_id = file_id, .byte_begin = u32(start), .byte_end = u32(end)};
    }
    cc::string_view text(isize start, isize end) const { return src.subview({.start = start, .end = end}); }

    void emit(token_kind kind, isize start)
    {
        out.tokens.push_back({.kind = kind, .span = span(start, p), .text = text(start, p)});
        if (kind != token_kind::whitespace && kind != token_kind::newline && kind != token_kind::line_comment
            && kind != token_kind::block_comment)
            line_has_content = true;
    }

    void diag(isize start, cc::string message)
    {
        out.diagnostics.push_back({.span = span(start, p), .message = cc::move(message)});
    }

    void run()
    {
        while (!eof())
            next();

        auto const end = src.size();
        out.tokens.push_back({.kind = token_kind::end_of_file, .span = span(end, end), .text = {}});
    }

    void next()
    {
        auto const start = p;
        char const c = cur();

        // Newlines: '\n', '\r', or '\r\n' as one break.
        if (c == '\n' || c == '\r')
        {
            ++p;
            if (c == '\r' && cur() == '\n')
                ++p;
            emit(token_kind::newline, start);
            line_has_content = false;
            return;
        }

        // Backslash-newline: line continuation, spliced away. Kept as trivia so spans stay gap-free.
        if (c == '\\' && (at(1) == '\n' || at(1) == '\r'))
        {
            p += (at(1) == '\r' && at(2) == '\n') ? 3 : 2;
            emit(token_kind::whitespace, start);
            return;
        }

        if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
        {
            while (cur() == ' ' || cur() == '\t' || cur() == '\f' || cur() == '\v')
                ++p;
            emit(token_kind::whitespace, start);
            return;
        }

        // Comments and the '/' operators.
        if (c == '/' && at(1) == '/')
        {
            lex_line_comment(start);
            return;
        }
        if (c == '/' && at(1) == '*')
        {
            lex_block_comment(start);
            return;
        }

        // A '#' as the first content on a line opens an opaque preprocessor directive.
        if (c == '#' && !line_has_content)
        {
            lex_directive(start);
            return;
        }

        // Plain string / char with no prefix.
        if (c == '"')
        {
            lex_string(start, /*raw*/ false);
            return;
        }
        if (c == '\'')
        {
            lex_char(start);
            return;
        }

        // Identifier, keyword, or a prefixed string/char literal (R"…", u8"…", L'…', …).
        if (is_ident_start(c))
        {
            lex_word_or_prefixed_literal(start);
            return;
        }

        // Numbers, incl. a leading-dot float like `.5`.
        if (is_digit(c) || (c == '.' && is_digit(at(1))))
        {
            lex_number(start);
            return;
        }

        if (lex_punctuation(start))
            return;

        // Anything else: one unknown byte, best-effort.
        ++p;
        emit(token_kind::unknown, start);
    }

    void lex_line_comment(isize start)
    {
        p += 2; // consume //
        while (!eof())
        {
            char const c = cur();
            if (c == '\n' || c == '\r')
                break;
            // Backslash-newline continues the comment onto the next physical line.
            if (c == '\\' && (at(1) == '\n' || at(1) == '\r'))
            {
                p += (at(1) == '\r' && at(2) == '\n') ? 3 : 2;
                continue;
            }
            ++p;
        }
        emit(token_kind::line_comment, start);
    }

    void lex_block_comment(isize start)
    {
        p += 2; // consume /*
        while (!eof())
        {
            if (cur() == '*' && at(1) == '/')
            {
                p += 2;
                emit(token_kind::block_comment, start);
                return;
            }
            ++p;
        }
        emit(token_kind::block_comment, start);
        diag(start, cc::string("unterminated block comment"));
    }

    void lex_directive(isize start)
    {
        ++p; // consume #
        while (!eof())
        {
            char const c = cur();
            if (c == '\n' || c == '\r')
                break;
            // A backslash-newline continues the directive onto the next line.
            if (c == '\\' && (at(1) == '\n' || at(1) == '\r'))
            {
                p += (at(1) == '\r' && at(2) == '\n') ? 3 : 2;
                continue;
            }
            // A // or /* comment ends the directive text (it is not part of the directive token).
            if (c == '/' && (at(1) == '/' || at(1) == '*'))
                break;
            ++p;
        }
        emit(token_kind::preprocessor_directive, start);
    }

    void lex_string(isize start, bool /*raw handled separately*/)
    {
        ++p; // opening "
        while (!eof())
        {
            char const c = cur();
            if (c == '\\')
            {
                p += 2; // skip the escaped char
                continue;
            }
            if (c == '"')
            {
                ++p;
                lex_optional_literal_suffix();
                emit(token_kind::string_literal, start);
                return;
            }
            if (c == '\n' || c == '\r')
                break; // a normal string cannot span a raw newline
            ++p;
        }
        emit(token_kind::string_literal, start);
        diag(start, cc::string("unterminated string literal"));
    }

    /// A raw string body: text is at the opening `"` of `"delim(...)delim"`. The prefix was already
    /// consumed by the caller (start points at the prefix).
    void lex_raw_string(isize start)
    {
        ++p; // opening "
        // Delimiter: chars up to '(' — no '(', ')', whitespace, or backslash; at most 16.
        auto const delim_begin = p;
        while (!eof() && cur() != '(' && p - delim_begin <= 16)
        {
            char const c = cur();
            if (c == ')' || c == ' ' || c == '\\' || c == '\t' || c == '\n' || c == '\r')
                break;
            ++p;
        }
        if (cur() != '(')
        {
            emit(token_kind::string_literal, start);
            diag(start, cc::string("malformed raw string delimiter"));
            return;
        }
        auto const delim = text(delim_begin, p);
        ++p; // consume (

        // Search for the closing )delim"
        while (!eof())
        {
            if (cur() == ')')
            {
                auto const after = p + 1;
                auto const close_len = delim.size();
                if (after + close_len < src.size() && text(after, after + close_len) == delim
                    && src[after + close_len] == '"')
                {
                    p = after + close_len + 1; // past the closing "
                    lex_optional_literal_suffix();
                    emit(token_kind::string_literal, start);
                    return;
                }
            }
            ++p;
        }
        emit(token_kind::string_literal, start);
        diag(start, cc::string("unterminated raw string literal"));
    }

    void lex_char(isize start)
    {
        ++p; // opening '
        while (!eof())
        {
            char const c = cur();
            if (c == '\\')
            {
                p += 2;
                continue;
            }
            if (c == '\'')
            {
                ++p;
                lex_optional_literal_suffix();
                emit(token_kind::char_literal, start);
                return;
            }
            if (c == '\n' || c == '\r')
                break;
            ++p;
        }
        emit(token_kind::char_literal, start);
        diag(start, cc::string("unterminated character literal"));
    }

    /// A user-defined-literal / standard suffix on a string or char literal (`sv`, `s`, `_km`, …).
    void lex_optional_literal_suffix()
    {
        if (is_ident_start(cur()))
            while (is_ident_cont(cur()))
                ++p;
    }

    void lex_word_or_prefixed_literal(isize start)
    {
        // Scan the whole identifier first.
        while (is_ident_cont(cur()))
            ++p;
        auto const word = text(start, p);

        // A string/char literal introduced by this word as an encoding/raw prefix?
        if (cur() == '"')
        {
            if (is_raw_prefix(word))
            {
                p = start; // rewind; lex_raw_string re-reads from the prefix
                lex_raw_string(start);
                return;
            }
            if (is_encoding_prefix(word))
            {
                lex_string(start, /*raw*/ false);
                return;
            }
        }
        else if (cur() == '\'' && is_encoding_prefix(word))
        {
            lex_char(start);
            return;
        }

        emit(is_keyword_spelling(word) ? token_kind::keyword : token_kind::identifier, start);
    }

    void lex_number(isize start)
    {
        // pp-number munch: starts with a digit or `.digit`; then digits, `.`, identifier chars, digit
        // separators `'`, and sign after an exponent (e/E for decimal, p/P for hex). Permissive — a
        // linter classifies, it does not validate.
        bool is_float = cur() == '.';
        ++p; // consume the first digit or the dot

        while (!eof())
        {
            char const c = cur();
            if (c == '\'' && is_ident_cont(at(1)))
            {
                p += 2; // digit separator between two digits/letters
                continue;
            }
            if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (at(1) == '+' || at(1) == '-'))
            {
                is_float = true;
                p += 2;
                continue;
            }
            if (c == '.')
            {
                is_float = true;
                ++p;
                continue;
            }
            if (is_ident_cont(c))
            {
                ++p;
                continue;
            }
            break;
        }
        emit(is_float ? token_kind::floating_literal : token_kind::integer_literal, start);
    }

    /// Maximal-munch punctuator, longest first. Returns false if the current char is not a punctuator.
    bool lex_punctuation(isize start)
    {
        static constexpr cc::string_view three[] = {"<<=", ">>=", "<=>", "...", "->*"};
        static constexpr cc::string_view two[] = {"::", "->", ".*", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
                                                  "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##"};
        static constexpr cc::string_view one = "{}()[];,.<>=+-*/%&|^~!?:#@$\\";

        auto const rest = src.subview_clamped(start, 3);
        for (auto const t : three)
            if (rest.size() >= 3 && rest.subview_clamped(0, 3) == t)
            {
                p = start + 3;
                emit(token_kind::punctuation, start);
                return true;
            }
        for (auto const t : two)
            if (rest.size() >= 2 && rest.subview_clamped(0, 2) == t)
            {
                p = start + 2;
                emit(token_kind::punctuation, start);
                return true;
            }
        if (one.contains(cur()))
        {
            ++p;
            emit(token_kind::punctuation, start);
            return true;
        }
        return false;
    }
};
} // namespace

cc::result<token_stream> lex(source_buffer const& buffer)
{
    scanner s;
    s.src = buffer.text();
    s.file_id = buffer.file_id();
    s.out.file_id = buffer.file_id();
    s.run();
    return cc::move(s.out);
}
} // namespace scl
