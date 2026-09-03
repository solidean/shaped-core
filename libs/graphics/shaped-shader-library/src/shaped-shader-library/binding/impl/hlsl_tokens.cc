#include <clean-core/string/format.hh>
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

/// The marker that separates an attribute from an ordinary comment.
/// Deliberately unlike anything a comment starts with, so no existing shader gains an attribute by accident.
constexpr cc::string_view k_annotation_marker = "//!>";

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
} // namespace

cc::result<cc::vector<slib::impl::hlsl_token>> slib::impl::lex_hlsl(cc::string_view hlsl)
{
    cc::vector<hlsl_token> tokens;

    auto const size = hlsl.size();
    isize i = 0;
    i32 line = 1;
    bool line_has_token = false;

    auto const emit = [&](hlsl_token_kind kind, cc::string_view text, isize offset, isize length)
    {
        tokens.push_back(
            {.kind = kind, .text = text, .offset = offset, .length = length, .line = line, .first_on_line = !line_has_token});
        line_has_token = true;
    };

    while (i < size)
    {
        char const c = hlsl[i];

        if (c == '\n')
        {
            ++line;
            line_has_token = false;
            ++i;
            continue;
        }

        if (is_space(c))
        {
            ++i;
            continue;
        }

        // A line comment runs to the end of the line and is dropped — unless it carries the attribute marker.
        if (c == '/' && i + 1 < size && hlsl[i + 1] == '/')
        {
            auto const start = i;
            auto end = i;
            while (end < size && hlsl[end] != '\n')
                ++end;

            auto const comment = hlsl.subview({.start = start, .end = end});
            if (comment.starts_with(k_annotation_marker))
                emit(hlsl_token_kind::annotation, trim(comment.subview(k_annotation_marker.size())), start, end - start);

            i = end;
            continue;
        }

        if (c == '/' && i + 1 < size && hlsl[i + 1] == '*')
        {
            auto const start_line = line;
            auto end = i + 2;
            while (end + 1 < size && !(hlsl[end] == '*' && hlsl[end + 1] == '/'))
            {
                if (hlsl[end] == '\n')
                    ++line;
                ++end;
            }
            if (end + 1 >= size)
                return cc::error(cc::format("line {}: unterminated block comment", start_line));

            i = end + 2;
            continue;
        }

        // A literal is skipped whole, so a `//` or a brace inside one never reaches the parser.
        if (c == '"' || c == '\'')
        {
            auto const start_line = line;
            auto end = i + 1;
            while (end < size && hlsl[end] != c)
            {
                if (hlsl[end] == '\n')
                    ++line;
                end += hlsl[end] == '\\' ? 2 : 1;
            }
            if (end >= size)
                return cc::error(cc::format("line {}: unterminated literal", start_line));

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

cc::result<slib::impl::annotation> slib::impl::parse_annotation(cc::string_view text, i32 line)
{
    annotation result;
    result.line = line;

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
        return cc::error(cc::format("line {}: an attribute must name what it is", line));

    while (true)
    {
        skip_spaces();
        if (i >= size)
            break;

        auto const word = read_word();
        if (word.empty())
            return cc::error(cc::format("line {}: unexpected '{}' in attribute '{}'", line,
                                        text.subview({.offset = i, .size = 1}), result.name));

        annotation_argument argument;
        if (i < size && text[i] == '=')
        {
            argument.key = word;
            ++i; // the '='

            if (i < size && text[i] == '(')
            {
                ++i; // the '('
                while (true)
                {
                    skip_spaces();
                    auto const value = read_word();
                    if (value.empty())
                        return cc::error(cc::format("line {}: '{}' has an empty value in its tuple", line, argument.key));
                    argument.values.push_back(value);

                    skip_spaces();
                    if (i >= size)
                        return cc::error(cc::format("line {}: '{}' opens a tuple it never closes", line, argument.key));
                    if (text[i] == ')')
                    {
                        ++i;
                        break;
                    }
                    if (text[i] != ',')
                        return cc::error(cc::format("line {}: expected ',' or ')' in '{}'", line, argument.key));
                    ++i;
                }
            }
            else
            {
                auto const value = read_word();
                if (value.empty())
                    return cc::error(cc::format("line {}: '{}' is missing its value", line, argument.key));
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
