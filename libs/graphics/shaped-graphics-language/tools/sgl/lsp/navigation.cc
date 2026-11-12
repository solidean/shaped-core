#include "features.hh"

#include <shaped-graphics-language/driver/format.hh>
#include <shaped-graphics-language/driver/impl/reference_reader.hh>
#include <shaped-graphics-language/driver/references.hh>

using namespace cc::primitive_defines;

namespace
{
/// A hover lists the rest of an overload set up to this many; `max` has dozens.
constexpr auto max_overloads_shown = isize(8);

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
    if (auto const& others = r.value().overloads; !others.empty())
    {
        markdown += "\n\n---\n\nOther overloads:\n\n```sgl";
        for (auto i = isize(0); i < others.size() && i < max_overloads_shown; ++i)
            markdown.appendf("\n{}", others[i]);
        markdown += "\n```";
        if (others.size() > max_overloads_shown)
            markdown.appendf("\n\nand {} more", others.size() - max_overloads_shown);
    }
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

cc::optional<lsp::signature_help> sgl_lsp::signature_help_of(analysis const& a, lsp::position at, lsp::position_encoding e)
{
    auto const files = a.module_files();
    auto const help = sgl::signature_help_at(files, a.module, a.user_file(), offset_at(a, at, e));
    if (!help.has_value())
        return {};
    auto out = lsp::signature_help{.active_signature = help.value().active_signature,
                                   .active_parameter = help.value().active_parameter};
    for (auto const& s : help.value().signatures)
    {
        auto info = lsp::signature_information{.label = s.label, .documentation = s.doc};
        for (auto const p : s.parameters)
            info.parameters.push_back(
                cc::string(cc::string_view(s.label).subview({.offset = isize(p.offset), .size = isize(p.length)})));
        out.signatures.push_back(cc::move(info));
    }
    return out;
}

cc::vector<lsp::location> sgl_lsp::references_of(analysis const& a,
                                                 lsp::position at,
                                                 bool include_declaration,
                                                 lsp::position_encoding e)
{
    auto const files = a.module_files();
    auto out = cc::vector<lsp::location>();
    for (auto const& o : sgl::occurrences_at(files, a.module, a.user_file(), offset_at(a, at, e)))
        if (include_declaration || !o.is_declaration)
            out.push_back({.uri = a.uri_of(o.file), .range = range_of(a, o.file, o.where, e)});
    return out;
}

namespace
{
/// Why what `r` names may not be renamed; empty where it may.
[[nodiscard]] cc::string rename_refusal(sgl_lsp::analysis const& a, cc::optional<sgl::reference> const& r)
{
    if (!r.has_value() || r.value().declaring_file < 0 || r.value().declared.empty())
        return "nothing declared is named here";
    auto const prelude = a.module.prelude_file_count();
    if (r.value().declaring_file < prelude && a.user_file() >= prelude)
        return "a name the prelude declares cannot be renamed";
    return {};
}

[[nodiscard]] lsp::answer<lsp::workspace_edit> refuse(cc::string message)
{
    return lsp::answer<lsp::workspace_edit>(
        cc::error(lsp::response_error{.code = lsp::error_code::request_failed, .message = cc::move(message)}));
}
} // namespace

cc::optional<lsp::prepare_rename_result> sgl_lsp::prepare_rename_of(analysis const& a,
                                                                    lsp::position at,
                                                                    lsp::position_encoding e)
{
    auto const r = reference_at(a, at, e);
    if (!rename_refusal(a, r).empty())
        return {};
    auto const where = r.value().where;
    auto const text = a.text_of(a.user_file());
    return lsp::prepare_rename_result{
        .range = range_of(a, a.user_file(), where, e),
        .placeholder = cc::string(text.subview({.offset = isize(where.offset), .size = isize(where.length)})),
    };
}

lsp::answer<lsp::workspace_edit> sgl_lsp::rename_of(analysis const& a,
                                                    lsp::position at,
                                                    cc::string_view new_name,
                                                    lsp::position_encoding e)
{
    auto is_name = !new_name.empty() && !(new_name[0] >= '0' && new_name[0] <= '9');
    for (auto const c : new_name)
        is_name = is_name && sgl::impl::is_word_char(c);
    if (!is_name)
        return refuse(cc::format("'{}' is no name", new_name));
    if (sgl::is_keyword(new_name))
        return refuse(cc::format("'{}' is a keyword", new_name));
    if (auto const why = rename_refusal(a, reference_at(a, at, e)); !why.empty())
        return refuse(why);

    auto const files = a.module_files();
    auto out = lsp::workspace_edit();
    for (auto const& o : sgl::occurrences_at(files, a.module, a.user_file(), offset_at(a, at, e)))
    {
        auto const uri = a.uri_of(o.file);
        auto* doc = static_cast<lsp::document_edits*>(nullptr);
        for (auto& d : out.changes)
            if (d.uri == uri)
                doc = &d;
        if (doc == nullptr)
        {
            out.changes.push_back({.uri = uri});
            doc = &out.changes.back();
        }
        doc->edits.push_back({.range = range_of(a, o.file, o.where, e), .new_text = cc::string(new_name)});
    }
    return out;
}

lsp::answer<cc::vector<lsp::text_edit>> sgl_lsp::formatting_of(analysis const& a, lsp::position_encoding e)
{
    auto const text = a.text_of(a.user_file());
    // the blocks a syntax error leaves as written carry its diagnostic already, so they need no word of their own
    auto const formatted = sgl::format_source(text);
    auto out = cc::vector<lsp::text_edit>();
    if (formatted.text != text)
        out.push_back({.range = range_of(a, a.user_file(), {.offset = 0, .length = u32(text.size())}, e),
                       .new_text = formatted.text});
    return out;
}
