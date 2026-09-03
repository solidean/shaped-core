#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-shader-library/binding/impl/hlsl_tokens.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] bool is_identifier_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_identifier_char(char c)
{
    return is_identifier_start(c) || is_digit(c);
}

[[nodiscard]] bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

[[nodiscard]] cc::string_view trim(cc::string_view sv)
{
    isize begin = 0;
    isize end = sv.size();
    while (begin < end && is_space(sv[begin]))
        ++begin;
    while (end > begin && is_space(sv[end - 1]))
        --end;
    return sv.subview({.start = begin, .end = end});
}

/// Advances past `word` and the whitespace after it, or reports that `text` does not start with it.
/// The word must end at a boundary, so `#pragma scope` is not a `#pragma sc` line.
[[nodiscard]] bool eat_word(cc::string_view& text, cc::string_view word)
{
    if (!text.starts_with(word))
        return false;
    if (text.size() > word.size() && is_identifier_char(text[word.size()]))
        return false;

    text.remove_prefix(word.size());
    while (!text.empty() && is_space(text[0]))
        text.remove_prefix(1);
    return true;
}
} // namespace

cc::string slib::impl::to_string(hlsl_location const& location)
{
    if (location.file.empty())
        return cc::format("line {}", location.line);
    return cc::format("{}:{}", location.file, location.line);
}

cc::result<cc::vector<slib::impl::hlsl_token>> slib::impl::lex_hlsl(cc::string_view hlsl)
{
    cc::vector<hlsl_token> tokens;

    auto const size = hlsl.size();
    isize i = 0;
    hlsl_location location;
    bool line_has_token = false;

    auto const rest_of_line = [&](isize from)
    {
        auto end = from;
        while (end < size && hlsl[end] != '\n')
            ++end;
        return end;
    };

    auto const emit = [&](hlsl_token_kind kind, cc::string_view text, isize offset, isize length)
    {
        tokens.push_back({.kind = kind,
                          .text = text,
                          .offset = offset,
                          .length = length,
                          .location = location,
                          .first_on_line = !line_has_token});
        line_has_token = true;
    };

    while (i < size)
    {
        char const c = hlsl[i];

        if (c == '\n')
        {
            ++location.line;
            line_has_token = false;
            ++i;
            continue;
        }

        if (is_space(c))
        {
            ++i;
            continue;
        }

        // A directive only counts as one when it opens its line, which is what the language requires of it too.
        if (c == '#' && !line_has_token)
        {
            auto const end = rest_of_line(i);
            auto tail = trim(hlsl.subview({.start = i + 1, .end = end}));

            if (eat_word(tail, "pragma") && eat_word(tail, "sc"))
            {
                emit(hlsl_token_kind::annotation, trim(tail), i, end - i);
                i = end;
                continue;
            }

            // `#line <n> ["file"]` is how the flattened source says where its text came from, so following it is
            // what lets an error name a file the author wrote rather than an offset into a flatten.
            tail = trim(hlsl.subview({.start = i + 1, .end = end}));
            if (eat_word(tail, "line"))
            {
                auto number = tail;
                isize digits = 0;
                while (digits < number.size() && is_digit(number[digits]))
                    ++digits;

                auto const parsed = cc::from_string<i32>(number.subview({.start = 0, .end = digits}));
                if (parsed.has_value())
                {
                    // The directive names the line AFTER it, and the newline below is what arrives there.
                    location.line = parsed.value() - 1;

                    auto quoted = trim(number.subview(digits));
                    if (quoted.starts_with('"'))
                    {
                        quoted.remove_prefix(1);
                        auto const closing = quoted.find('"');
                        if (closing >= 0)
                            location.file = quoted.subview({.start = 0, .end = closing});
                    }

                    i = end;
                    continue;
                }
            }
        }

        // A line comment runs to the end of the line and carries nothing the parser needs.
        if (c == '/' && i + 1 < size && hlsl[i + 1] == '/')
        {
            i = rest_of_line(i);
            continue;
        }

        if (c == '/' && i + 1 < size && hlsl[i + 1] == '*')
        {
            auto const opened_at = location;
            auto end = i + 2;
            while (end + 1 < size && !(hlsl[end] == '*' && hlsl[end + 1] == '/'))
            {
                if (hlsl[end] == '\n')
                    ++location.line;
                ++end;
            }
            if (end + 1 >= size)
                return cc::error(cc::format("{}: unterminated block comment", to_string(opened_at)));

            i = end + 2;
            continue;
        }

        // A literal is skipped whole, so a `//` or a brace inside one never reaches the parser.
        if (c == '"' || c == '\'')
        {
            auto const opened_at = location;
            auto end = i + 1;
            while (end < size && hlsl[end] != c)
            {
                if (hlsl[end] == '\n')
                    ++location.line;
                end += hlsl[end] == '\\' ? 2 : 1;
            }
            if (end >= size)
                return cc::error(cc::format("{}: unterminated literal", to_string(opened_at)));

            i = end + 1;
            continue;
        }

        if (is_identifier_start(c))
        {
            auto end = i;
            while (end < size && is_identifier_char(hlsl[end]))
                ++end;

            emit(hlsl_token_kind::identifier, hlsl.subview({.start = i, .end = end}), i, end - i);
            i = end;
            continue;
        }

        if (is_digit(c) || (c == '.' && i + 1 < size && is_digit(hlsl[i + 1])))
        {
            auto end = i;
            while (end < size && (is_identifier_char(hlsl[end]) || hlsl[end] == '.'))
            {
                // An exponent's sign belongs to the literal; a `-` anywhere else is an operator.
                auto const prev = hlsl[end];
                ++end;
                if ((prev == 'e' || prev == 'E') && end < size && (hlsl[end] == '+' || hlsl[end] == '-'))
                    ++end;
            }

            emit(hlsl_token_kind::number, hlsl.subview({.start = i, .end = end}), i, end - i);
            i = end;
            continue;
        }

        emit(hlsl_token_kind::punctuation, hlsl.subview({.offset = i, .size = 1}), i, 1);
        ++i;
    }

    return tokens;
}

cc::result<slib::impl::annotation> slib::impl::parse_annotation(cc::string_view text, hlsl_location const& location)
{
    annotation result;
    result.location = location;

    auto const size = text.size();
    isize i = 0;

    auto const skip_spaces = [&]
    {
        while (i < size && is_space(text[i]))
            ++i;
    };

    // A word is anything up to whitespace or one of the grammar's own characters, which is what keeps
    // `clamp_edge`, `9` and `less` all readable as one token without a second lexer.
    auto const read_word = [&]() -> cc::string_view
    {
        auto const start = i;
        while (i < size && !is_space(text[i]) && text[i] != '=' && text[i] != '(' && text[i] != ')' && text[i] != ',')
            ++i;
        return text.subview({.start = start, .end = i});
    };

    skip_spaces();
    result.name = read_word();
    if (result.name.empty())
        return cc::error(cc::format("{}: an attribute must name what it is", to_string(location)));

    while (true)
    {
        skip_spaces();
        if (i >= size)
            break;

        auto const word = read_word();
        if (word.empty())
            return cc::error(cc::format("{}: unexpected '{}' in attribute '{}'", to_string(location),
                                        text.subview({.offset = i, .size = 1}), result.name));

        // Whitespace around `=` is tolerated: the flatten reproduces a pragma's tokens, and nothing guarantees it
        // reproduces the spacing between them.
        skip_spaces();

        annotation_argument argument;
        if (i < size && text[i] == '=')
        {
            argument.key = word;
            ++i; // the '='
            skip_spaces();

            if (i < size && text[i] == '(')
            {
                ++i; // the '('
                while (true)
                {
                    skip_spaces();
                    auto const value = read_word();
                    if (value.empty())
                        return cc::error(
                            cc::format("{}: '{}' has an empty value in its tuple", to_string(location), argument.key));
                    argument.values.push_back(value);

                    skip_spaces();
                    if (i >= size)
                        return cc::error(
                            cc::format("{}: '{}' opens a tuple it never closes", to_string(location), argument.key));
                    if (text[i] == ')')
                    {
                        ++i;
                        break;
                    }
                    if (text[i] != ',')
                        return cc::error(cc::format("{}: expected ',' or ')' in '{}'", to_string(location), argument.key));
                    ++i;
                }
            }
            else
            {
                auto const value = read_word();
                if (value.empty())
                    return cc::error(cc::format("{}: '{}' is missing its value", to_string(location), argument.key));
                argument.values.push_back(value);
            }
        }
        else
        {
            argument.values.push_back(word);
        }

        result.arguments.push_back(cc::move(argument));
    }

    return result;
}
