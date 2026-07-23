#include "token.hh"

namespace scl
{
bool is_keyword_spelling(cc::string_view word)
{
    // A pragmatic subset: the keywords the parser reasons about (record heads, member specifiers,
    // scope) plus the common ones, so `keyword` vs `identifier` is meaningful. Not the full C++ list.
    static constexpr cc::string_view keywords[] = {
        "alignas",
        "alignof",
        "and",
        "asm",
        "auto",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "nullptr",
        "operator",
        "or",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
    };
    for (auto const kw : keywords)
        if (word == kw)
            return true;
    return false;
}

cc::string_view token_kind_name(token_kind k)
{
    switch (k)
    {
    case token_kind::end_of_file:
        return "end_of_file";
    case token_kind::unknown:
        return "unknown";
    case token_kind::identifier:
        return "identifier";
    case token_kind::keyword:
        return "keyword";
    case token_kind::integer_literal:
        return "integer_literal";
    case token_kind::floating_literal:
        return "floating_literal";
    case token_kind::char_literal:
        return "char_literal";
    case token_kind::string_literal:
        return "string_literal";
    case token_kind::punctuation:
        return "punctuation";
    case token_kind::line_comment:
        return "line_comment";
    case token_kind::block_comment:
        return "block_comment";
    case token_kind::whitespace:
        return "whitespace";
    case token_kind::newline:
        return "newline";
    case token_kind::preprocessor_directive:
        return "preprocessor_directive";
    }
    return "?";
}
} // namespace scl
