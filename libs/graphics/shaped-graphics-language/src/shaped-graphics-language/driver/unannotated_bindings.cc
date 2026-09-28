#include "unannotated_bindings.hh"

#include <clean-core/algorithm/sort.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>

namespace
{
/// Whether `value` is a call whose callee resolved to a function or constructor named as `type` is.
[[nodiscard]] bool is_call_named_after(sgl::ast::file_ast const& ast,
                                       sgl::check::checked_module const& m,
                                       sgl::i32 file,
                                       sgl::ast::expr_id value,
                                       sgl::check::type_id type)
{
    if (!sgl::ast::is_valid(value))
        return false;
    auto const* call = ast.at(value).node.try_as<sgl::ast::call>();
    if (call == nullptr || !sgl::ast::is_valid(call->callee))
        return false;
    auto const& tables = m.files[file];
    if (sgl::ast::index_of(call->callee) >= tables.target_of.size())
        return false;
    auto const& target = tables.target_at(call->callee);
    auto const is_call
        = target.kind == sgl::check::target_kind::constructor || target.kind == sgl::check::target_kind::overload;
    return is_call && sgl::check::is_valid(target.symbol) && m.at(target.symbol).name == m.name_of(type);
}
} // namespace

cc::vector<sgl::unannotated_binding> sgl::unannotated_bindings(parsed_file const&,
                                                               ast::file_ast const& ast,
                                                               check::checked_module const& m,
                                                               i32 file_index)
{
    auto out = cc::vector<unannotated_binding>();
    if (file_index < 0 || file_index >= m.files.size())
        return out;
    auto const& tables = m.files[file_index];
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
        out.push_back(
            {.name = n->where, .type = type, .is_type_named = is_call_named_after(ast, m, file_index, l->value, type)});
    }
    // statements are stored in the order the builder finished them, which is not always source order
    cc::sort_by(out, [](unannotated_binding const& b) { return b.name.offset; });
    return out;
}
