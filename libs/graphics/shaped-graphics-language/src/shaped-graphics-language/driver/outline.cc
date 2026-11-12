#include "outline.hh"

#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;

namespace
{
struct outliner
{
    parsed_file const& file;
    ast::file_ast const& a;

    [[nodiscard]] source_span where_of(ast::decl const& d) const
    {
        return is_valid(d.form) ? file.at(d.form).where : source_span();
    }

    /// The declaration's first line, trimmed, without the colon that opens its block.
    [[nodiscard]] cc::string detail_of(source_span where) const
    {
        auto const text = cc::string_view(file.source);
        auto end = isize(where.offset);
        while (end < text.size() && end < isize(where.end()) && text[end] != '\n' && text[end] != '\r')
            ++end;
        auto line = text.subview({.start = isize(where.offset), .end = end});
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line = line.subview({.start = 0, .end = line.size() - 1});
        if (line.ends_with(":") && !line.ends_with("::"))
            line = line.subview({.start = 0, .end = line.size() - 1});
        return cc::string(line);
    }

    [[nodiscard]] outline_entry entry(cc::string_view name, outline_kind kind, ast::decl const& d, source_span name_span) const
    {
        auto const where = where_of(d);
        return {.name = cc::string(name), .kind = kind, .where = where, .name_span = name_span, .detail = detail_of(where)};
    }

    void add_members(outline_entry& parent, ast::range_of<ast::decl_id> members, bool is_binding)
    {
        for (auto const id : a.at(members))
            add(parent.children, a.at(id), true, is_binding);
    }

    void add(cc::vector<outline_entry>& out, ast::decl const& d, bool is_member, bool is_binding)
    {
        auto const named = [&](source_span name, outline_kind kind)
        {
            out.push_back(entry(file.text_of(name), kind, d, name));
            return &out.back();
        };
        if (auto const* x = d.node.try_as<ast::fun_decl>())
        {
            auto const is_method = is_member || !x->extended_type.empty();
            auto* e = named(x->name, is_method ? outline_kind::method : outline_kind::function);
            if (!x->extended_type.empty())
                e->name = cc::string(file.text_of(x->extended_type)) + "." + e->name;
        }
        else if (auto const* x = d.node.try_as<ast::property_decl>())
        {
            auto* e = named(x->name, outline_kind::property);
            if (!x->extended_type.empty())
                e->name = cc::string(file.text_of(x->extended_type)) + "." + e->name;
        }
        else if (auto const* x = d.node.try_as<ast::struct_decl>())
            add_members(*named(x->name, outline_kind::structure), x->members, false);
        else if (auto const* x = d.node.try_as<ast::enum_decl>())
            add_members(*named(x->name, outline_kind::enumeration), x->members, false);
        else if (auto const* x = d.node.try_as<ast::enum_case_decl>())
            named(x->name, outline_kind::enum_case);
        else if (auto const* x = d.node.try_as<ast::binding_decl>())
            add_members(*named(x->name, outline_kind::binding), x->members, true);
        else if (auto const* x = d.node.try_as<ast::field_decl>())
        {
            if (ast::is_valid(x->field) && !a.at(x->field).name.empty())
                named(a.at(x->field).name, outline_kind::field);
        }
        else if (auto const* x = d.node.try_as<ast::type_decl>())
            named(x->name, outline_kind::type);
        else if (auto const* x = d.node.try_as<ast::const_decl>())
            named(x->name, outline_kind::constant);
        else if (auto const* x = d.node.try_as<ast::sampler_decl>())
            named(x->name, is_binding ? outline_kind::field : outline_kind::sampler);
        else if (auto const* x = d.node.try_as<ast::pipeline_decl>())
        {
            if (!x->name.empty())
                named(x->name, outline_kind::pipeline);
            else
                out.push_back(entry("pipeline", outline_kind::pipeline, d, where_of(d)));
        }
        else if (auto const* x = d.node.try_as<ast::test_decl>())
            out.push_back(entry("test", outline_kind::test, d, x->keyword));
        else if (auto const* x = d.node.try_as<ast::module_decl>())
        {
            if (ast::is_valid(x->path) && is_valid(a.at(x->path).form))
            {
                auto const span = file.at(a.at(x->path).form).where;
                named(span, outline_kind::module);
            }
        }
    }
};
} // namespace

cc::vector<outline_entry> sgl::outline_of(parsed_file const& file, ast::file_ast const& ast)
{
    auto o = outliner{.file = file, .a = ast};
    auto out = cc::vector<outline_entry>();
    for (auto const id : ast.at(ast.declarations))
        o.add(out, ast.at(id), false, false);
    return out;
}

cc::vector<fold_range> sgl::folds_of(parsed_file const& file)
{
    auto out = cc::vector<fold_range>();
    auto const& lines = file.lines;
    for (auto i = isize(0); i < lines.size(); ++i)
    {
        if (!is_valid(lines[i].first_child))
            continue;
        // pre-order: the subtree is every following line deeper than this one, its next sibling's line and on excluded
        auto last = i;
        auto end = lines.size();
        for (auto up = line_id(i32(i)); is_valid(up); up = lines[index_of(up)].parent)
            if (is_valid(lines[index_of(up)].next_sibling))
            {
                end = index_of(lines[index_of(up)].next_sibling);
                break;
            }
        for (auto j = i + 1; j < end; ++j)
            if (lines[j].kind != line_kind::blank)
                last = j;
        if (last > i)
            out.push_back({.first_line = u32(i),
                           .last_line = u32(last),
                           .is_comment = lines[i].token_count > 0
                                      && file.at(token_id(i32(lines[i].first_token))).kind == token_kind::comment});
    }
    // a run of comment lines at one indentation, each a line of its own
    auto run_start = isize(-1);
    auto const is_comment_line = [&](isize i)
    {
        auto const& l = lines[i];
        return l.kind == line_kind::code && l.token_count == 1
            && file.at(token_id(i32(l.first_token))).kind == token_kind::comment && !is_valid(l.first_child);
    };
    for (auto i = isize(0); i <= lines.size(); ++i)
    {
        auto const is_comment = i < lines.size() && is_comment_line(i)
                             && (run_start < 0 || lines[i].indent_columns == lines[run_start].indent_columns);
        if (is_comment && run_start < 0)
            run_start = i;
        else if (!is_comment && run_start >= 0)
        {
            if (i - 1 > run_start)
                out.push_back({.first_line = u32(run_start), .last_line = u32(i - 1), .is_comment = true});
            run_start = i < lines.size() && is_comment_line(i) ? i : -1;
        }
    }
    return out;
}
