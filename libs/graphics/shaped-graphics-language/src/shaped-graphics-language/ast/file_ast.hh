#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/ast/decl.hh>
#include <shaped-graphics-language/ast/expr.hh>
#include <shaped-graphics-language/ast/stmt.hh>
#include <shaped-graphics-language/source/diagnostic.hh>

namespace sgl::ast::impl
{
template <class T>
[[nodiscard]] cc::span<T const> slice(cc::vector<T> const& all, range_of<T> r)
{
    return cc::span<T const>(all).subspan({.offset = r.first, .size = r.count});
}

template <class T>
[[nodiscard]] bool is_equal(cc::vector<T> const& a, cc::vector<T> const& b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}
} // namespace sgl::ast::impl

/// The AST of one file, as one value beside the `parsed_file` it was built from.
///
/// Per file and name-free: no name is ever looked up, so wherever only lookup can answer the neutral reading is kept.
/// Every node names the form it came from, and every span points into that file's `source`.
/// A node is an id into one of the three family arrays and a child list is a `range_of` one of the side arrays,
/// so the value copies and compares whole and holds no pointer into itself.
struct sgl::ast::file_ast
{
    cc::vector<expr> exprs;
    cc::vector<stmt> stmts;
    cc::vector<decl> decls;

    cc::vector<field> fields;
    cc::vector<argument> arguments;
    cc::vector<attribute> attributes;
    cc::vector<case_arm> case_arms;
    cc::vector<if_branch> if_branches;
    cc::vector<setting> settings;
    /// The child lists whose elements are nodes or tokens of their own.
    cc::vector<expr_id> expr_lists;
    cc::vector<stmt_id> stmt_lists;
    cc::vector<decl_id> decl_lists;
    cc::vector<token_id> token_lists;

    /// What the AST pass itself found; the syntactic diagnostics stay in the `parsed_file`.
    cc::vector<diagnostic> diagnostics;

    /// The file's declarations, in source order.
    range_of<decl_id> declarations;

    /// An id is the way to a node, and the id's type picks the array.
    /// The id must be valid and must name a node that exists.
    [[nodiscard]] expr const& at(expr_id id) const { return exprs[index_of(id)]; }
    [[nodiscard]] stmt const& at(stmt_id id) const { return stmts[index_of(id)]; }
    [[nodiscard]] decl const& at(decl_id id) const { return decls[index_of(id)]; }
    [[nodiscard]] field const& at(field_id id) const { return fields[index_of(id)]; }

    /// A range is the way to a child list, and its element type picks the array.
    [[nodiscard]] cc::span<field const> at(range_of<field> r) const { return impl::slice(fields, r); }
    [[nodiscard]] cc::span<argument const> at(range_of<argument> r) const { return impl::slice(arguments, r); }
    [[nodiscard]] cc::span<attribute const> at(range_of<attribute> r) const { return impl::slice(attributes, r); }
    [[nodiscard]] cc::span<case_arm const> at(range_of<case_arm> r) const { return impl::slice(case_arms, r); }
    [[nodiscard]] cc::span<if_branch const> at(range_of<if_branch> r) const { return impl::slice(if_branches, r); }
    [[nodiscard]] cc::span<setting const> at(range_of<setting> r) const { return impl::slice(settings, r); }
    [[nodiscard]] cc::span<expr_id const> at(range_of<expr_id> r) const { return impl::slice(expr_lists, r); }
    [[nodiscard]] cc::span<stmt_id const> at(range_of<stmt_id> r) const { return impl::slice(stmt_lists, r); }
    [[nodiscard]] cc::span<decl_id const> at(range_of<decl_id> r) const { return impl::slice(decl_lists, r); }
    [[nodiscard]] cc::span<token_id const> at(range_of<token_id> r) const { return impl::slice(token_lists, r); }

    [[nodiscard]] bool operator==(file_ast const& rhs) const
    {
        return impl::is_equal(exprs, rhs.exprs) && impl::is_equal(stmts, rhs.stmts) && impl::is_equal(decls, rhs.decls)
            && impl::is_equal(fields, rhs.fields) && impl::is_equal(arguments, rhs.arguments)
            && impl::is_equal(attributes, rhs.attributes) && impl::is_equal(case_arms, rhs.case_arms)
            && impl::is_equal(if_branches, rhs.if_branches) && impl::is_equal(settings, rhs.settings)
            && impl::is_equal(expr_lists, rhs.expr_lists) && impl::is_equal(stmt_lists, rhs.stmt_lists)
            && impl::is_equal(decl_lists, rhs.decl_lists) && impl::is_equal(token_lists, rhs.token_lists)
            && impl::is_equal(diagnostics, rhs.diagnostics) && declarations == rhs.declarations;
    }
};
