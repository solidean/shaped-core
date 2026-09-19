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

bool is_space(char c)
{
    return c == ' ' || c == '\t';
}
bool is_quote(char c)
{
    return c == '"' || c == '\'' || c == '`';
}

/// Tokenizes one code line; `at` and `end` are byte offsets into the file's source.
struct code_line_tokenizer
{
    parsed_file& file;
    cc::string_view text;
    isize at;
    isize end;

    /// The quote this line leaves open for its children and its next sibling, 0 for none.
    char open_quote = 0;
    bool is_comment_only = false;

    [[nodiscard]] char peek(isize offset = 0) const { return at + offset < end ? text[at + offset] : '\0'; }
    [[nodiscard]] bool at_comment() const { return peek() == '/' && peek(1) == '/'; }

    void emit(token_kind kind, isize from, isize to)
    {
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

    /// `expected_close` is the quote a multi-line string on the previous sibling is waiting for, 0 for none.
    void run(char expected_close)
    {
        skip_space();
        auto const first_token = file.tokens.size();

        if (expected_close != 0)
        {
            if (peek() == expected_close)
            {
                emit(token_kind::quote_close, at, at + 1);
                ++at;
            }
            else
                report(diagnostic_kind::missing_string_end, at, at + 1);
        }

        while (true)
        {
            skip_space();
            if (at >= end)
                break;

            auto const c = text[at];
            if (at_comment())
            {
                is_comment_only = file.tokens.size() == first_token;
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
    }

    void symbol()
    {
        auto const start = at;
        ++at;
        while (at < end)
        {
            auto const c = text[at];
            if (!is_symbol_start(c) && c != '\'')
                break;
            ++at;
        }
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
        auto close = start + 1;
        while (close < end && text[close] != quote)
            close += text[close] == '\\' ? 2 : 1;

        emit(token_kind::quote_open, start, start + 1);
        if (close < end)
        {
            if (close > start + 1)
                emit(token_kind::string_body, start + 1, close);
            emit(token_kind::quote_close, close, close + 1);
            at = close + 1;
            return;
        }

        at = start + 1;
        skip_space();
        if (at >= end)
        {
            open_quote = quote;
            return;
        }

        // Closing it at the end of the line is the one reasonable reading, so that is the token we produce.
        report(diagnostic_kind::undelimited_string, start, start + 1);
        emit(token_kind::string_body, start + 1, end);
        at = end;
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

    // Per line: what it hands to its children, and the quote its previous sibling left for it to close.
    auto hands_down = cc::vector<child_mode>::create_filled(line_count, child_mode::code);
    auto must_close = cc::vector<char>::create_filled(line_count, char(0));

    for (auto index = isize(0); index < line_count; ++index)
    {
        auto& l = file.lines[index];
        if (l.kind == line_kind::blank)
            continue;

        auto const mode = l.parent >= 0 ? hands_down[l.parent] : child_mode::code;
        auto const content = isize(l.text.offset + l.indent_bytes);
        auto const end = isize(l.text.end());

        l.first_token = u32(file.tokens.size());
        if (mode != child_mode::code)
        {
            auto const is_comment = mode == child_mode::comment;
            l.kind = is_comment ? line_kind::comment : line_kind::string_content;
            hands_down[index] = mode;
            file.tokens.push_back({
                .kind = is_comment ? token_kind::comment : token_kind::string_body,
                .where = {.offset = u32(content), .length = u32(end - content)},
            });
            l.token_count = 1;
            continue;
        }

        auto tokenizer = code_line_tokenizer{.file = file, .text = text, .at = content, .end = end};
        tokenizer.run(must_close[index]);
        l.token_count = u32(file.tokens.size()) - l.first_token;

        if (tokenizer.is_comment_only)
            hands_down[index] = child_mode::comment;

        if (tokenizer.open_quote != 0)
        {
            hands_down[index] = child_mode::string_content;

            auto closer = l.next_sibling;
            while (closer >= 0 && file.lines[closer].kind == line_kind::blank)
                closer = file.lines[closer].next_sibling;

            if (closer >= 0)
                must_close[closer] = tokenizer.open_quote;
            else
            {
                auto const& opener = file.tokens.back();
                file.diagnostics.push_back({
                    .kind = diagnostic_kind::missing_string_end,
                    .level = default_severity_of(diagnostic_kind::missing_string_end),
                    .where = opener.where,
                });
            }
        }
    }
}
