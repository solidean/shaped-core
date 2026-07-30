#include "python_lexer.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>

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
    // Non-ASCII bytes are let through: Python identifiers may be unicode, and treating a UTF-8 lead byte
    // as an identifier character keeps a word whole rather than splitting it into `unknown` tokens.
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || u8(c) >= 0x80;
}
bool is_ident_cont(char c)
{
    return is_ident_start(c) || is_digit(c);
}

/// A string prefix is any combination of r/b/u/f in either case, immediately before the quote.
bool is_string_prefix(cc::string_view s)
{
    if (s.size() > 3)
        return false;
    for (auto const c : s)
        if (c != 'r' && c != 'R' && c != 'b' && c != 'B' && c != 'u' && c != 'U' && c != 'f' && c != 'F')
            return false;
    return true;
}

/// The single-pass tokenizer, mirroring the C++ lexer's shape: a cursor over the file text, every token
/// spanning a byte range, the whole file tiled gap-free.
///
/// Indentation is the one piece of structure it tracks.
/// The level of a logical line is decided at its
/// first content token, so a blank or comment-only line — which Python's own grammar ignores — leaves the
/// stack untouched.
struct scanner
{
    cc::string_view src;
    u32 file_id = 0;
    isize p = 0;
    isize bracket_depth = 0;   // a newline inside (), [] or {} does not end a logical line
    bool at_line_start = true; // the next content token opens a logical line, so its column decides indent
    cc::vector<u32> levels;    // the indentation stack, always starting at column 0
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
    }

    void emit_marker(token_kind kind) { out.tokens.push_back({.kind = kind, .span = span(p, p), .text = {}}); }

    void diag(isize start, cc::string message)
    {
        out.diagnostics.push_back({.span = span(start, p), .message = cc::move(message)});
    }

    void run()
    {
        levels.push_back(0);

        while (!eof())
            next();

        // Close every open block at end of file, so the dedents balance the indents exactly.
        while (levels.size() > 1)
        {
            levels.remove_back();
            emit_marker(token_kind::dedent);
        }

        auto const end = src.size();
        out.tokens.push_back({.kind = token_kind::end_of_file, .span = span(end, end), .text = {}});
    }

    /// The visual column of `p` on its own line, tabs advancing to the next multiple of 8.
    u32 column_here() const
    {
        auto line_begin = p;
        while (line_begin > 0 && src[line_begin - 1] != '\n')
            --line_begin;

        u32 col = 0;
        for (auto i = line_begin; i < p; ++i)
            col = src[i] == '\t' ? (col / 8 + 1) * 8 : col + 1;
        return col;
    }

    /// Called before the first content token of a logical line: reconcile the stack with its column.
    void open_logical_line()
    {
        at_line_start = false;

        auto const col = column_here();
        if (col > levels.back())
        {
            levels.push_back(col);
            emit_marker(token_kind::indent);
            return;
        }
        // An inconsistent dedent (a column matching no open level) is not our problem to diagnose —
        // popping to the nearest enclosing level keeps the stream balanced, which is all a rule reads.
        while (levels.size() > 1 && col < levels.back())
        {
            levels.remove_back();
            emit_marker(token_kind::dedent);
        }
    }

    void next()
    {
        auto const start = p;
        char const c = cur();

        if (c == '\n' || c == '\r')
        {
            if (c == '\r' && at(1) == '\n')
                ++p;
            ++p;
            emit(token_kind::newline, start);
            if (bracket_depth == 0)
                at_line_start = true;
            return;
        }

        if (c == ' ' || c == '\t' || c == '\f')
        {
            while (cur() == ' ' || cur() == '\t' || cur() == '\f')
                ++p;
            emit(token_kind::whitespace, start);
            return;
        }

        // A backslash-newline joins lines; it is trivia and leaves the logical line open.
        if (c == '\\' && (at(1) == '\n' || at(1) == '\r'))
        {
            ++p;
            if (cur() == '\r' && at(1) == '\n')
                ++p;
            ++p;
            emit(token_kind::whitespace, start);
            return;
        }

        if (c == '#')
        {
            while (!eof() && cur() != '\n' && cur() != '\r')
                ++p;
            emit(token_kind::line_comment, start);
            return;
        }

        // Everything past here is content, so this is where a logical line's indentation is settled.
        if (at_line_start)
            open_logical_line();

        if (is_ident_start(c))
        {
            auto const word_start = p;
            while (is_ident_cont(cur()))
                ++p;
            auto const word = text(word_start, p);

            if ((cur() == '"' || cur() == '\'') && is_string_prefix(word))
            {
                scan_string(start);
                return;
            }
            emit(is_python_keyword_spelling(word) ? token_kind::keyword : token_kind::identifier, start);
            return;
        }

        if (c == '"' || c == '\'')
        {
            scan_string(start);
            return;
        }

        if (is_digit(c) || (c == '.' && is_digit(at(1))))
        {
            scan_number(start);
            return;
        }

        if (c == '(' || c == '[' || c == '{')
            ++bracket_depth;
        else if (c == ')' || c == ']' || c == '}')
            bracket_depth = bracket_depth > 0 ? bracket_depth - 1 : 0;

        scan_punctuation(start);
    }

    /// A string literal, from its prefix (already consumed) through its closing quote.
    /// Triple-quoted strings run across lines; a backslash escapes the next character in both forms,
    /// raw strings included — in Python a raw string still cannot end on a lone backslash.
    void scan_string(isize start)
    {
        char const quote = cur();
        auto const triple = at(1) == quote && at(2) == quote;
        p += triple ? 3 : 1;

        while (!eof())
        {
            if (cur() == '\\')
            {
                p += p + 1 < src.size() ? 2 : 1;
                continue;
            }
            if (cur() == quote)
            {
                if (!triple)
                {
                    ++p;
                    emit(token_kind::string_literal, start);
                    return;
                }
                if (at(1) == quote && at(2) == quote)
                {
                    p += 3;
                    emit(token_kind::string_literal, start);
                    return;
                }
            }
            if (!triple && (cur() == '\n' || cur() == '\r'))
                break; // a single-quoted string never crosses a line
            ++p;
        }

        emit(token_kind::string_literal, start);
        diag(start, cc::string("unterminated string literal"));
    }

    void scan_number(isize start)
    {
        auto floating = false;
        if (cur() == '0' && (at(1) == 'x' || at(1) == 'X' || at(1) == 'b' || at(1) == 'B' || at(1) == 'o' || at(1) == 'O'))
        {
            p += 2;
            while (is_ident_cont(cur()))
                ++p;
        }
        else
        {
            while (is_digit(cur()) || cur() == '_')
                ++p;
            if (cur() == '.' && (is_digit(at(1)) || !is_ident_start(at(1))))
            {
                floating = true;
                ++p;
                while (is_digit(cur()) || cur() == '_')
                    ++p;
            }
            if (cur() == 'e' || cur() == 'E')
            {
                auto const after = at(1) == '+' || at(1) == '-' ? at(2) : at(1);
                if (is_digit(after))
                {
                    floating = true;
                    p += at(1) == '+' || at(1) == '-' ? 2 : 1;
                    while (is_digit(cur()) || cur() == '_')
                        ++p;
                }
            }
            if (cur() == 'j' || cur() == 'J')
                ++p;
        }
        emit(floating ? token_kind::floating_literal : token_kind::integer_literal, start);
    }

    /// Maximal munch over Python's operators, longest spelling first.
    void scan_punctuation(isize start)
    {
        static constexpr cc::string_view three[] = {"**=", "//=", ">>=", "<<=", "..."};
        static constexpr cc::string_view two[] = {"**", "//", ">>", "<<", "<=", ">=", "==", "!=", "+=", "-=",
                                                  "*=", "/=", "%=", "&=", "|=", "^=", ":=", "->", "@="};

        auto const rest = src.subview({.start = p, .end = src.size()});
        for (auto const op : three)
            if (rest.size() >= 3 && rest.subview({.start = 0, .end = 3}) == op)
            {
                p += 3;
                emit(token_kind::punctuation, start);
                return;
            }
        for (auto const op : two)
            if (rest.size() >= 2 && rest.subview({.start = 0, .end = 2}) == op)
            {
                p += 2;
                emit(token_kind::punctuation, start);
                return;
            }

        ++p;
        emit(token_kind::punctuation, start);
    }
};
} // namespace

bool is_python_keyword_spelling(cc::string_view word)
{
    static constexpr cc::string_view keywords[] = {
        "False",  "None",     "True",   "and", "as",   "assert", "async",  "await",    "break", "case",
        "class",  "continue", "def",    "del", "elif", "else",   "except", "finally",  "for",   "from",
        "global", "if",       "import", "in",  "is",   "lambda", "match",  "nonlocal", "not",   "or",
        "pass",   "raise",    "return", "try", "type", "while",  "with",   "yield",
    };
    for (auto const kw : keywords)
        if (word == kw)
            return true;
    return false;
}

cc::result<token_stream> lex_python(source_buffer const& buffer)
{
    scanner s = {.src = buffer.text(), .file_id = buffer.file_id()};
    s.out.file_id = buffer.file_id();
    s.run();
    return cc::move(s.out);
}
} // namespace scl
