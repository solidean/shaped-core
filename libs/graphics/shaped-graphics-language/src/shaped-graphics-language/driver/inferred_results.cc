#include "inferred_results.hh"

#include <clean-core/algorithm/sort.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

cc::vector<sgl::inferred_result> sgl::inferred_results(parsed_file const& file,
                                                       ast::file_ast const& ast,
                                                       check::checked_module const& m,
                                                       i32 file_index)
{
    auto out = cc::vector<inferred_result>();
    if (file_index < 0 || file_index >= m.files.size())
        return out;

    // the `=>` is the operator leaf right before the body's form, and forms link only forward
    auto before = cc::vector<form_id>::create_filled(file.forms.size(), form_id::none);
    for (auto i = isize(0); i < file.forms.size(); ++i)
        if (is_valid(file.forms[i].next_sibling))
            before[index_of(file.forms[i].next_sibling)] = form_id(i32(i));
    auto const arrow_before = [&](form_id body) -> source_span
    {
        if (!is_valid(body) || !is_valid(before[index_of(body)]))
            return {};
        auto const& f = file.at(before[index_of(body)]);
        if (f.kind != form_kind::op || !file.text_of(f.where).starts_with("=>"))
            return {};
        return f.where;
    };

    for (auto const& s : m.symbols)
    {
        if (s.file != file_index || s.kind != check::symbol_kind::function || s.info < 0 || !ast::is_valid(s.declaration)
            || ast::index_of(s.declaration) >= ast.decls.size() || s.state != check::symbol_state::checked)
            continue;
        auto const type = m.functions[s.info].result;
        if (!check::is_valid(type) || m.at(type).kind == check::type_kind::error)
            continue;

        auto const& node = ast.at(s.declaration).node;
        auto arrow = source_span();
        auto is_writable = true;
        if (auto const* const f = node.try_as<ast::fun_decl>())
        {
            if (ast::is_valid(f->return_type) || f->body.kind != ast::body_kind::arrow)
                continue;
            arrow = arrow_before(f->body.form);
        }
        else if (auto const* const p = node.try_as<ast::property_decl>())
        {
            if (ast::is_valid(p->return_type))
                continue;
            arrow = arrow_before(p->body.form);
            is_writable = !p->extended_type.empty();
        }
        if (!arrow.empty())
            out.push_back({.arrow = arrow, .type = type, .is_writable = is_writable});
    }
    cc::sort_by(out, [](inferred_result const& r) { return r.arrow.offset; });
    return out;
}
