#include "tokenizer.hh"

#include <clean-core/common/assert.hh>

namespace
{
using namespace sgl;

/// What a line makes of the lines below it.
enum class child_mode : u8
{
    code,
    comment,
    string_content,
};

/// What a line hands to its children; everything past `mode` only matters for string content.
struct handed_down
{
    child_mode mode = child_mode::code;
    /// Only a double-quoted string that is not raw interpolates.
    bool interpolates = false;
    /// The indentation content is measured from: the opening line's columns plus four.
    u32 content_columns = 0;
};

/// What a line leaves for its next sibling to close, `quote == 0` for nothing.
struct owed_closer
{
    char quote = 0;
    /// A `"""` opener is closed by `"""`.
    bool is_triple = false;
};

bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

bool is_quote(char c)
{
    return c == '"' || c == '\'' || c == '`';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// A name inside a string is narrower than a symbol: `'`, `@`, `#` and `\` are text there, so "$name's" works.
bool is_name_start(char c)
{
    return is_word_char(c) && !is_digit(c);
}

/// Tokenizes one line; `at` and `end` are byte offsets into the file's source.
struct line_tokenizer
{
    parsed_file& file;
    cc::string_view text;
    isize at;
    isize end;
    /// Whether the line owns indented lines, which the line tree knows before any token is read.
    bool has_children = false;

    /// What this line leaves open for its children and its next sibling.
    owed_closer opened;
    bool opened_interpolates = false;
    bool is_comment_only = false;

    [[nodiscard]] char peek(isize offset = 0) const { return at + offset < end ? text[at + offset] : '\0'; }
    [[nodiscard]] bool at_comment() const { return peek() == '/' && peek(1) == '/'; }

    [[nodiscard]] bool is_blank_from(isize from) const
    {
        for (auto i = from; i < end; ++i)
            if (!is_space(text[i]))
                return false;
        return true;
    }

    void emit(token_kind kind, isize from, isize to)
    {
        if (to > from)
            file.tokens.push_back({.kind = kind, .where = {.offset = u32(from), .length = u32(to - from)}});
    }

    void report(diagnostic_kind kind, isize from, isize to)
    {
        file.diagnostics.push_back({
            .kind = kind,
            .level = default_severity_of(kind),
            .where = {.offset = u32(from), .length = u32(to - from)},
        });
    }

    void skip_space()
    {
        while (at < end && is_space(text[at]))
            ++at;
    }

    /// `closer` is what a multi-line string on the previous sibling is waiting for.
    void run_code(owed_closer closer)
    {
        skip_space();
        auto const first_token = file.tokens.size();

        if (closer.quote != 0)
        {
            auto const width = closer.is_triple && peek(1) == closer.quote && peek(2) == closer.quote ? 3 : 1;
            if (peek() == closer.quote)
            {
                emit(token_kind::quote_close, at, at + width);
                at += width;
            }
            else
                report(diagnostic_kind::missing_string_end, at, at + 1);
        }

        while (true)
        {
            skip_space();
            if (at >= end)
                break;
            if (at_comment())
                is_comment_only = file.tokens.size() == first_token;
            step();
        }
    }

    /// One token of code, or one whole quoted literal.
    void step()
    {
        auto const c = text[at];
        if (at_comment())
        {
            emit(token_kind::comment, at, end);
            at = end;
        }
        else if (is_quote(c))
            quoted(c);
        else if (is_symbol_start(c))
            symbol();
        else if (c == '.' && peek(1) == '.')
            operator_run(2);
        else if (is_operator_char(c))
            operator_run(0);
        else
            punctuation_or_error(c);
    }

    void symbol()
    {
        auto const start = at;
        ++at;
        while (at < end && (is_symbol_start(text[at]) || text[at] == '\''))
            ++at;
        auto const is_wildcard = at - start == 1 && text[start] == '_';
        emit(is_wildcard ? token_kind::wildcard : token_kind::symbol, start, at);
    }

    /// `prefix` bytes are taken unconditionally, which is how `..` opens an operator though `.` is not an operator character.
    void operator_run(isize prefix)
    {
        auto const start = at;
        at += prefix;
        while (at < end && is_operator_char(text[at]) && !at_comment())
            ++at;

        auto const spelling = text.subview({.start = start, .end = at});
        auto kind = token_kind::op;
        if (spelling == "->")
            kind = token_kind::arrow;
        else if (spelling == "=>")
            kind = token_kind::double_arrow;
        emit(kind, start, at);
    }

    void quoted(char quote)
    {
        auto const start = at;
        auto const interpolates = quote == '"';

        // `"""` with nothing after it is kept free for raw strings.
        if (peek(1) == quote && peek(2) == quote && is_blank_from(start + 3))
        {
            emit(token_kind::quote_open, start, start + 3);
            report(diagnostic_kind::reserved_string_opener, start, start + 3);
            opened = {.quote = quote, .is_triple = true};
            at = end;
            return;
        }

        emit(token_kind::quote_open, start, start + 1);
        at = start + 1;

        // Nothing after the quote: the children are the string.
        if (is_blank_from(at))
        {
            opened = {.quote = quote};
            opened_interpolates = interpolates;
            at = end;
            return;
        }

        // Exactly one name after the quote, with lines below it: a tagged opener, kept free for embedded languages.
        // Without lines below it is what it looks like, a one-word string somebody has not closed yet.
        if (has_children && is_name_start(peek()))
        {
            auto name_end = at;
            while (name_end < end && is_word_char(text[name_end]))
                ++name_end;
            if (is_blank_from(name_end))
            {
                emit(token_kind::symbol, at, name_end);
                report(diagnostic_kind::reserved_string_opener, start, name_end);
                opened = {.quote = quote};
                opened_interpolates = interpolates;
                at = end;
                return;
            }
        }

        // Closing it at the end of the line is the one reasonable reading, so that is what the tokens say.
        if (!body(quote, interpolates, true))
            report(diagnostic_kind::undelimited_string, start, start + 1);
    }

    /// The inside of a string up to `quote`, or up to the end of the line when `quote` is 0 or never comes.
    /// Returns whether the quote was found.
    bool body(char quote, bool interpolates, bool has_escapes)
    {
        auto piece = at;
        while (at < end)
        {
            auto const c = text[at];
            if (quote != 0 && c == quote)
            {
                emit(token_kind::string_body, piece, at);
                emit(token_kind::quote_close, at, at + 1);
                ++at;
                return true;
            }

            if (has_escapes && c == '\\')
                escape();
            else if (interpolates && c == '$' && peek(1) == '$')
                at += 2;
            else if (interpolates && c == '$' && (peek(1) == '(' || is_name_start(peek(1))))
            {
                emit(token_kind::string_body, piece, at);
                interpolation();
                piece = at;
            }
            else
            {
                if (interpolates && c == '$')
                    report(diagnostic_kind::stray_dollar, at, at + 1);
                ++at;
            }
        }
        emit(token_kind::string_body, piece, at);
        return false;
    }

    void escape()
    {
        auto const start = at;
        auto const c = peek(1);
        at = at + 2 < end ? at + 2 : end;

        switch (c)
        {
        case '\\':
        case '"':
        case '\'':
        case '`':
        case 'n':
        case 'r':
        case 't':
        case '0':
            return;
        case 'u':
        {
            // `\u{…}` with one to six hex digits; anything else about it is not an escape.
            auto i = at;
            if (i < end && text[i] == '{')
            {
                ++i;
                auto const digits = i;
                while (i < end && is_hex_digit(text[i]))
                    ++i;
                if (i < end && text[i] == '}' && i > digits && i - digits <= 6)
                {
                    at = i + 1;
                    return;
                }
            }
            break;
        }
        default:
            break;
        }
        report(diagnostic_kind::unknown_escape, start, at);
    }

    /// `$name`, `$name.member.member` or `$(expr)`; `at` is on the `$`.
    void interpolation()
    {
        emit(token_kind::dollar, at, at + 1);
        ++at;

        if (peek() != '(')
        {
            name();
            while (peek() == '.' && is_name_start(peek(1)))
            {
                emit(token_kind::dot, at, at + 1);
                ++at;
                name();
            }
            return;
        }

        // The parentheses hold code, nested strings included, and they end on this line whatever happens.
        auto const open = at;
        auto depth = 0;
        while (at < end)
        {
            skip_space();
            if (at >= end)
                break;
            auto const c = text[at];
            if (c == '(')
                ++depth;
            if (c == ')')
                --depth;
            step();
            if (c == ')' && depth == 0)
                return;
        }
        report(diagnostic_kind::missing_closer, open, open + 1);
    }

    void name()
    {
        auto const start = at;
        while (at < end && is_word_char(text[at]))
            ++at;
        emit(token_kind::symbol, start, at);
    }

    void punctuation_or_error(char c)
    {
        auto kind = token_kind::error;
        auto length = isize(1);
        switch (c)
        {
        case '.':
            kind = token_kind::dot;
            break;
        case ',':
            kind = token_kind::comma;
            break;
        case ';':
            kind = token_kind::semicolon;
            break;
        case '(':
            kind = token_kind::round_open;
            break;
        case ')':
            kind = token_kind::round_close;
            break;
        case '[':
            kind = token_kind::square_open;
            break;
        case ']':
            kind = token_kind::square_close;
            break;
        case '{':
            kind = token_kind::curly_open;
            break;
        case '}':
            kind = token_kind::curly_close;
            break;
        case ':':
            kind = peek(1) == ':' ? token_kind::double_colon : token_kind::colon;
            length = kind == token_kind::double_colon ? 2 : 1;
            break;
        default:
            break;
        }

        if (kind != token_kind::error)
        {
            emit(kind, at, at + length);
            at += length;
            return;
        }

        auto const start = at;
        while (at < end && !recognizes(text[at]))
            ++at;
        emit(token_kind::error, start, at);
        report(diagnostic_kind::unknown_character, start, at);
    }

    /// True for every byte some rule above starts a token or a gap with; an error token runs until the next one.
    [[nodiscard]] static bool recognizes(char c)
    {
        if (is_space(c) || is_quote(c) || is_symbol_start(c) || is_operator_char(c))
            return true;
        switch (c)
        {
        case '.':
        case ',':
        case ';':
        case ':':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
            return true;
        default:
            return false;
        }
    }
};
} // namespace

bool sgl::is_word_char(char c)
{
    auto const u = static_cast<unsigned char>(c);
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}

bool sgl::is_symbol_start(char c)
{
    return is_word_char(c) || c == '@' || c == '#' || c == '\\';
}

bool sgl::is_operator_char(char c)
{
    switch (c)
    {
    case '!':
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '=':
    case '<':
    case '>':
    case '?':
    case '&':
    case '^':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

void sgl::tokenize(parsed_file& file)
{
    CC_ASSERT(file.tokens.empty(), "a file is tokenized once");

    auto const line_count = file.lines.size();
    auto const text = cc::string_view(file.source);

    // Per line: what it hands to its children, and what its previous sibling left for it to close.
    auto hands_down = cc::vector<handed_down>::create_defaulted(line_count);
    auto must_close = cc::vector<owed_closer>::create_defaulted(line_count);

    for (auto index = isize(0); index < line_count; ++index)
    {
        auto& l = file.lines[index];
        if (l.kind == line_kind::blank)
            continue;

        auto const inherited = l.parent >= 0 ? hands_down[l.parent] : handed_down{};
        auto const content = isize(l.text.offset + l.indent_bytes);
        auto const end = isize(l.text.end());
        auto has_children = false;
        for (auto child = l.first_child; child >= 0 && !has_children; child = file.lines[child].next_sibling)
            has_children = file.lines[child].kind != line_kind::blank;
        auto tokenizer
            = line_tokenizer{.file = file, .text = text, .at = content, .end = end, .has_children = has_children};

        l.first_token = u32(file.tokens.size());
        if (inherited.mode == child_mode::comment)
        {
            l.kind = line_kind::comment;
            hands_down[index] = inherited;
            tokenizer.emit(token_kind::comment, content, end);
        }
        else if (inherited.mode == child_mode::string_content)
        {
            l.kind = line_kind::string_content;
            hands_down[index] = inherited;
            if (l.indent_columns < inherited.content_columns)
                tokenizer.report(diagnostic_kind::underindented_string_content, content, content + 1);
            // Nothing in here can close the string, so nothing needs escaping.
            tokenizer.body(0, inherited.interpolates, false);
        }
        else
        {
            tokenizer.run_code(must_close[index]);
            if (tokenizer.is_comment_only)
                hands_down[index].mode = child_mode::comment;
        }
        l.token_count = u32(file.tokens.size()) - l.first_token;

        if (tokenizer.opened.quote == 0 || inherited.mode != child_mode::code)
            continue;

        l.opens_string = true;
        hands_down[index] = {
            .mode = child_mode::string_content,
            .interpolates = tokenizer.opened_interpolates,
            .content_columns = l.indent_columns + 4,
        };

        auto closer = l.next_sibling;
        while (closer >= 0 && file.lines[closer].kind == line_kind::blank)
            closer = file.lines[closer].next_sibling;

        if (closer >= 0)
            must_close[closer] = tokenizer.opened;
        else
        {
            auto const kind = diagnostic_kind::missing_string_end;
            file.diagnostics.push_back(
                {.kind = kind, .level = default_severity_of(kind), .where = file.tokens.back().where});
        }
    }
}
