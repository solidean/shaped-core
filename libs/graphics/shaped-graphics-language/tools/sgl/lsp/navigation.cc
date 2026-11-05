#include "features.hh"

#include <shaped-graphics-language/driver/references.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] u32 offset_at(sgl_lsp::analysis const& a, lsp::position at, lsp::position_encoding e)
{
    auto const user = a.user_file();
    return u32(a.index_of(user).offset_of(a.text_of(user), at, e));
}

[[nodiscard]] cc::optional<sgl::reference> reference_at(sgl_lsp::analysis const& a,
                                                        lsp::position at,
                                                        lsp::position_encoding e)
{
    auto const files = a.module_files();
    return sgl::reference_at(files, a.module, a.user_file(), offset_at(a, at, e));
}

[[nodiscard]] lsp::completion_item_kind kind_of(sgl::token_class c)
{
    using k = lsp::completion_item_kind;
    switch (c)
    {
    case sgl::token_class::struct_:
        return k::struct_;
    case sgl::token_class::enum_:
        return k::enum_;
    case sgl::token_class::type:
        return k::type_parameter;
    case sgl::token_class::function:
        return k::function;
    case sgl::token_class::method:
        return k::method;
    case sgl::token_class::property:
        return k::property;
    case sgl::token_class::field:
    case sgl::token_class::binding_member:
        return k::field;
    case sgl::token_class::enum_case:
        return k::enum_member;
    case sgl::token_class::constant:
        return k::constant;
    case sgl::token_class::keyword:
    case sgl::token_class::control:
        return k::keyword;
    default:
        return k::variable;
    }
}
} // namespace

cc::optional<lsp::hover> sgl_lsp::hover_of(analysis const& a, lsp::position at, lsp::position_encoding e)
{
    auto const r = reference_at(a, at, e);
    if (!r.has_value())
        return {};
    auto markdown = cc::format("```sgl\n{}\n```", r.value().signature);
    if (!r.value().doc.empty())
        markdown.appendf("\n\n{}", r.value().doc);
    return lsp::hover{.markdown = cc::move(markdown), .range = range_of(a, a.user_file(), r.value().where, e)};
}

cc::optional<lsp::location> sgl_lsp::definition_of(analysis const& a, lsp::position at, lsp::position_encoding e)
{
    auto const r = reference_at(a, at, e);
    if (!r.has_value() || r.value().declaring_file < 0 || r.value().declared.empty())
        return {};
    auto const file = r.value().declaring_file;
    return lsp::location{.uri = a.uri_of(file), .range = range_of(a, file, r.value().declared, e)};
}

cc::vector<lsp::completion_item> sgl_lsp::completions_of(analysis const& a, lsp::position at, lsp::position_encoding e)
{
    auto const files = a.module_files();
    auto out = cc::vector<lsp::completion_item>();
    for (auto& c : sgl::completions_at(files, a.module, a.user_file(), offset_at(a, at, e)))
        out.push_back({.label = cc::move(c.label), .kind = kind_of(c.cls), .detail = cc::move(c.detail)});
    return out;
}
