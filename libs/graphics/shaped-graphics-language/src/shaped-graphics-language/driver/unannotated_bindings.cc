#include "unannotated_bindings.hh"

#include <clean-core/algorithm/sort.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>

cc::vector<sgl::unannotated_binding> sgl::unannotated_bindings(ast::file_ast const& ast,
                                                               check::checked_module const& m,
                                                               i32 file)
{
    auto out = cc::vector<unannotated_binding>();
    if (file < 0 || file >= m.files.size())
        return out;
    auto const& tables = m.files[file];
    for (auto const& s : ast.stmts)
    {
        auto const* l = s.node.try_as<ast::let_stmt>();
        if (l == nullptr || ast::is_valid(l->type) || !ast::is_valid(l->pattern)
            || ast::index_of(l->pattern) >= tables.type_of.size())
            continue;
        auto const* n = ast.at(l->pattern).node.try_as<ast::name>();
        if (n == nullptr)
            continue;
        auto const type = tables.type_at(l->pattern);
        if (!check::is_valid(type) || m.at(type).kind == check::type_kind::error)
            continue;
        out.push_back({.name = n->where, .type = type});
    }
    // statements are stored in the order the builder finished them, which is not always source order
    cc::sort_by(out, [](unannotated_binding const& b) { return b.name.offset; });
    return out;
}
