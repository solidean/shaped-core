#pragma once

#include <shaped-graphics-language/check/flat.hh>

namespace sgl::check::impl
{
/// Beyond this nesting a tree is no program, and a walk stops descending so that a cyclic tree cannot exhaust the stack.
constexpr int k_max_depth = 200;

[[nodiscard]] inline bool is_known(flat_entry_point const& e, flat_expr_id id)
{
    return is_valid(id) && index_of(id) < e.exprs.size();
}
[[nodiscard]] inline bool is_known(flat_entry_point const& e, flat_stmt_id id)
{
    return is_valid(id) && index_of(id) < e.stmts.size();
}
[[nodiscard]] inline bool is_known(flat_entry_point const& e, local_id id)
{
    return is_valid(id) && index_of(id) < e.locals.size();
}
[[nodiscard]] inline bool is_known(flat_entry_point const& e, label_id id)
{
    return is_valid(id) && index_of(id) < e.labels.size();
}
[[nodiscard]] inline bool is_known(flat_entry_point const& e, ast::range_of<flat_expr_id> r)
{
    return isize(r.first) + isize(r.count) <= e.expr_lists.size();
}
[[nodiscard]] inline bool is_known(flat_entry_point const& e, ast::range_of<flat_stmt_id> r)
{
    return isize(r.first) + isize(r.count) <= e.stmt_lists.size();
}

/// Calls `fn(flat_expr_id)` for every direct operand of `x` in evaluation order; the body of a block expression is no operand.
/// A list that reaches outside the tree has no operands here.
template <class Fn>
void for_each_operand(flat_entry_point const& e, flat_expr const& x, Fn&& fn)
{
    if (auto const* const member = x.node.try_as<flat_member>())
        fn(member->object);
    else if (auto const* const construct = x.node.try_as<flat_construct>())
    {
        if (is_known(e, construct->arguments))
            for (auto const a : e.at(construct->arguments))
                fn(a);
    }
    else if (auto const* const call = x.node.try_as<flat_call>())
    {
        if (is_known(e, call->arguments))
            for (auto const a : e.at(call->arguments))
                fn(a);
    }
    else if (auto const* const n = x.node.try_as<flat_not>())
        fn(n->operand);
    else if (auto const* const a = x.node.try_as<flat_and>())
    {
        fn(a->lhs);
        fn(a->rhs);
    }
    else if (auto const* const o = x.node.try_as<flat_or>())
    {
        fn(o->lhs);
        fn(o->rhs);
    }
}

/// Calls `fn(flat_expr_id)` for every expression `s` holds directly, in evaluation order; an absent one is skipped.
template <class Fn>
void for_each_expr_of(flat_stmt const& s, Fn&& fn)
{
    auto const visit = [&](flat_expr_id id)
    {
        if (is_valid(id))
            fn(id);
    };
    s.node.visit([&](flat_let const& n) { visit(n.value); }, //
                 [&](flat_var const& n) { visit(n.value); },
                 [&](flat_assign const& n)
                 {
                     visit(n.place);
                     visit(n.value);
                 },
                 [&](flat_print const& n) { visit(n.value); },  //
                 [&](flat_eval const& n) { visit(n.value); },   //
                 [&](flat_if const& n) { visit(n.condition); }, //
                 [&](flat_block const&) {},                     //
                 [&](flat_leave const& n) { visit(n.value); },  //
                 [&](flat_loop const&) {},                      //
                 [&](flat_while const& n) { visit(n.condition); },
                 [&](flat_for const& n)
                 {
                     visit(n.first);
                     visit(n.end);
                 },
                 [&](flat_continue const&) {}, //
                 [&](flat_once const&) {},     //
                 [&](flat_break const&) {},    //
                 [&](flat_return const& n) { visit(n.value); });
}

/// Calls `fn(ast::range_of<flat_stmt_id>)` for every statement list `s` holds directly.
template <class Fn>
void for_each_body_of(flat_stmt const& s, Fn&& fn)
{
    if (auto const* const i = s.node.try_as<flat_if>())
    {
        fn(i->then_body);
        fn(i->else_body);
    }
    else if (auto const* const b = s.node.try_as<flat_block>())
        fn(b->body);
    else if (auto const* const l = s.node.try_as<flat_loop>())
        fn(l->body);
    else if (auto const* const w = s.node.try_as<flat_while>())
        fn(w->body);
    else if (auto const* const f = s.node.try_as<flat_for>())
        fn(f->body);
    else if (auto const* const o = s.node.try_as<flat_once>())
        fn(o->body);
}
} // namespace sgl::check::impl
