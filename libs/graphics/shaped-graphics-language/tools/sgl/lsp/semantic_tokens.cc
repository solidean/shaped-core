#include "features.hh"

#include <shaped-graphics-language/driver/classify.hh>

using namespace cc::primitive_defines;

namespace
{
// The legend: LSP's standard types, so every theme colours them.
// A custom type would go uncoloured until a theme opted in; the one custom modifier is styled by the extension instead.
enum token_type : u32
{
    t_type,
    t_struct,
    t_enum,
    t_enum_member,
    t_function,
    t_method,
    t_property,
    t_variable,
    t_parameter,
    t_keyword,
    t_number,
    t_string,
    t_comment,
    t_operator,
    t_decorator,
    token_type_count,
};

constexpr cc::string_view token_type_names[]
    = {"type",      "struct",  "enum",   "enumMember", "function", "method",   "property", "variable",
       "parameter", "keyword", "number", "string",     "comment",  "operator", "decorator"};
static_assert(sizeof(token_type_names) / sizeof(token_type_names[0]) == token_type_count);

enum token_modifier : u32
{
    m_declaration = 1u << 0,
    m_readonly = 1u << 1,
    m_static = 1u << 2,
    m_default_library = 1u << 3,
    m_documentation = 1u << 4,
    /// Not LSP's: the extension underlines it, as rust-analyzer does for what can change.
    m_mutable = 1u << 5,
};

constexpr cc::string_view token_modifier_names[]
    = {"declaration", "readonly", "static", "defaultLibrary", "documentation", "mutable"};

struct mapped
{
    bool is_shown = false;
    u32 type = 0;
    u32 modifiers = 0;
};

/// THE mapping from SGL's classes to the legend, in one place so it is easy to revisit once seen in an editor.
/// Declaration and prelude modifiers are added from the span's flags afterwards.
[[nodiscard]] mapped map_class(sgl::classified_span const& s)
{
    using sgl::token_class;
    switch (s.cls)
    {
    case token_class::keyword:
        return {true, t_keyword, 0};
    case token_class::control:
        // Left to the grammar, which scopes them `keyword.control`: a semantic token would replace that with the plain
        // keyword colour, since a theme styles the `keyword` type before any modifier mapping the extension contributes.
        return {};
    case token_class::number:
        return {true, t_number, 0};
    case token_class::string:
        return {true, t_string, 0};
    case token_class::comment:
        return {true, t_comment, 0};
    case token_class::doc_comment:
        return {true, t_comment, m_documentation};
    case token_class::op:
        return {true, t_operator, 0};
    case token_class::attribute:
        return {}; // left to the grammar for the same reason: a theme styles `decorator` as a function
    case token_class::struct_:
        // a builtin struct such as `vec3` reads as a type, a user's as a struct
        if (s.is_from_prelude)
            return {true, t_type, 0};
        return {true, t_struct, 0};
    case token_class::enum_:
        return {true, t_enum, 0};
    case token_class::type:
        return {true, t_type, 0};
    case token_class::function:
        return {true, t_function, 0};
    case token_class::method:
        return {true, t_method, 0};
    case token_class::property:
        return {true, t_property, m_readonly};
    case token_class::field:
        return {true, t_property, 0};
    case token_class::binding:
        return {true, t_variable, m_static | m_readonly};
    case token_class::binding_member:
        return {true, t_property, 0};
    case token_class::enum_case:
        // a case of a prelude enum is a constant of the language, `true` and `false` above all
        if (s.is_from_prelude)
            return {true, t_keyword, 0};
        return {true, t_enum_member, 0};
    case token_class::constant:
        return {true, t_variable, m_static | m_readonly};
    case token_class::pipeline:
        return {true, t_variable, m_static | m_readonly};
    case token_class::parameter:
        return {true, t_parameter, 0};
    case token_class::argument:
        return {true, t_parameter, 0};
    case token_class::local:
        return {true, t_variable, m_readonly};
    case token_class::mutable_local:
        return {true, t_variable, m_mutable};
    case token_class::self_:
        return {true, t_keyword, 0};
    case token_class::name:
        return {}; // left to the grammar: an unresolved name is no class worth claiming
    }
    return {};
}
} // namespace

lsp::semantic_tokens_legend sgl_lsp::semantic_tokens_legend()
{
    auto out = lsp::semantic_tokens_legend();
    for (auto const n : token_type_names)
        out.token_types.push_back(cc::string(n));
    for (auto const n : token_modifier_names)
        out.token_modifiers.push_back(cc::string(n));
    return out;
}

lsp::semantic_tokens sgl_lsp::semantic_tokens_of(analysis const& a, lsp::position_encoding e)
{
    auto const user = a.user_file();
    auto const spans = sgl::classify(
        a.file, a.ast, {.module = &a.module, .file = user, .prelude_file_count = i32(the_prelude().files.size())});
    auto const text = a.text_of(user);
    auto const& index = a.index_of(user);

    auto out = lsp::semantic_tokens();
    auto last_line = i32(0);
    auto last_start = i32(0);
    auto const emit = [&](lsp::position start, i32 length, mapped const& m)
    {
        if (length <= 0)
            return;
        auto const delta_line = start.line - last_line;
        auto const delta_start = delta_line == 0 ? start.character - last_start : start.character;
        out.data.push_back(u32(delta_line));
        out.data.push_back(u32(delta_start));
        out.data.push_back(u32(length));
        out.data.push_back(m.type);
        out.data.push_back(m.modifiers);
        last_line = start.line;
        last_start = start.character;
    };

    for (auto const& s : spans)
    {
        auto m = map_class(s);
        if (!m.is_shown)
            continue;
        if (s.is_declaration)
            m.modifiers |= m_declaration;
        if (s.is_from_prelude)
            m.modifiers |= m_default_library;

        auto const start = index.position_of(text, s.where.offset, e);
        auto const end = index.position_of(text, s.where.end(), e);
        if (start.line == end.line)
        {
            emit(start, end.character - start.character, m);
            continue;
        }
        // a token that spans lines is one token per line, since a client need not support multi-line ones
        for (auto line = start.line; line <= end.line; ++line)
        {
            auto const from = line == start.line ? start : lsp::position{.line = line, .character = 0};
            auto const line_end
                = index.position_of(text, index.offset_of(text, {.line = line, .character = 1 << 30}, e), e);
            auto const to = line == end.line ? end : line_end;
            emit(from, to.character - from.character, m);
        }
    }
    return out;
}
