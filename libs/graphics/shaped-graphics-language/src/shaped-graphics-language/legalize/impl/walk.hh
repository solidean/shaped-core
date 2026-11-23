#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics-language/check/flat.hh>

namespace sgl::check::impl
{
/// Beyond this nesting a tree is no program, and a walk stops descending so that a cyclic tree cannot exhaust the stack.
///
/// The number is bounded by the smallest stack a walk may run on, rather than by what a program might legitimately nest.
/// An unoptimized sanitizer build spends ~8 KiB on one `interpret` level, and when a `cc::async_thread_pool` worker had
/// the platform's default 512 KiB, a limit of 200 let the stack die at about level 60 before this guard fired, which is
/// the crash it exists to prevent.
/// Every worker has had cc::native_thread's 8 MiB since 2026-10-21, as Linux's and macOS's main threads have.
/// A level of the deepest walk, the check pass's `check_expr` through a call, takes ~6 KiB of stack in an optimized
/// Windows build and ~14 KiB unoptimized under ASan, read off the frames: 132 levels fit 8 MiB in every build, and the
/// 1 MiB of a Windows main thread or of Emscripten (tools/cmake/Emscripten.cmake) only in an optimized one.
/// So a host compiling SGL on a thread of its own gives it 2 MiB, or 1 MiB where the build is optimized.
/// Raise it only against that budget; the depth test compiles a tree at the limit on a pool worker.
///
/// It is a language limit as well: a top-level `let x = a + b + …` of 128 terms is past it.
/// The check pass refuses such an entry point as `nesting-too-deep` (CHK-268), so no walk here ever meets the hole it
/// would leave behind.
/// The real fix is iterative walks over an explicit work stack for `interpret`'s `eval` and the legalizer's expression
/// walks, with a cycle caught by an on-path bit rather than by depth; the limit could then be far higher.
/// 132 is 128 levels of a program (CHK-268) and the 4 a guard adds, about 1 MiB of an 8 MiB stack at 8 KiB a level.
/// It was 44 until a port's path tracer, with every call inlined, nested 48 levels (D94 of Quest3D's SGL migration).
constexpr int k_max_depth = 132;

/// How many levels a guard of a robustness profile nests its operands deeper (LEGAL-65).
constexpr int k_guard_depth_reserve = 4;

/// How deep a program's tree may nest (CHK-268): the walks' limit, less what a guard adds, so every site a profile
/// guards stands where every walk still reaches it.
constexpr int k_max_program_depth = k_max_depth - k_guard_depth_reserve;

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
[[nodiscard]] inline bool is_known(flat_entry_point const& e, ast::range_of<flat_arm> r)
{
    return isize(r.first) + isize(r.count) <= e.arms.size();
}

/// Calls `fn(flat_expr_id)` for every direct operand of `x` in evaluation order; the body of a block expression is no operand.
/// A list that reaches outside the tree has no operands here.
template <class Fn>
void for_each_operand(flat_entry_point const& e, flat_expr const& x, Fn&& fn)
{
    if (auto const* const member = x.node.try_as<flat_member>())
        fn(member->object);
    else if (auto const* const element = x.node.try_as<flat_buffer_element>())
    {
        fn(element->buffer);
        fn(element->index);
    }
    else if (auto const* const length = x.node.try_as<flat_buffer_length>())
        fn(length->buffer);
    else if (auto const* const written = x.node.try_as<flat_written_word>())
        fn(written->word);
    else if (auto const* const element = x.node.try_as<flat_element>())
    {
        fn(element->object);
        fn(element->index);
    }
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
void for_each_expr_of(flat_entry_point const& e, flat_stmt const& s, Fn&& fn)
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
                 [&](flat_print const& n)
                 {
                     if (is_known(e, n.values))
                         for (auto const value : e.at(n.values))
                             visit(value);
                 },
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
                 [&](flat_discard const&) {},  //
                 [&](flat_once const&) {},     //
                 [&](flat_break const&) {},    //
                 [&](flat_case const& n) { visit(n.scrutinee); }, [&](flat_switch const& n) { visit(n.scrutinee); },
                 [&](flat_return const& n) { visit(n.value); },
                 // its condition is the statements of its body
                 [&](flat_check const&) {});
}

/// Calls `fn(ast::range_of<flat_expr_id>)` for the patterns of every arm of `s`, in the order they are tried.
/// They are no operand of the statement: a pattern behind the one that matched never runs (EVAL-67).
template <class Fn>
void for_each_pattern_of(flat_entry_point const& e, flat_stmt const& s, Fn&& fn)
{
    auto const arms_of = [&](ast::range_of<flat_arm> arms)
    {
        if (is_known(e, arms))
            for (auto const& arm : e.at(arms))
                fn(arm.patterns);
    };
    if (auto const* const c = s.node.try_as<flat_case>())
        arms_of(c->arms);
    else if (auto const* const sw = s.node.try_as<flat_switch>())
        arms_of(sw->arms);
}

/// Calls `fn(ast::range_of<flat_stmt_id>)` for every statement list `s` holds directly, an arm's body included.
template <class Fn>
void for_each_body_of(flat_entry_point const& e, flat_stmt const& s, Fn&& fn)
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
    else if (auto const* const k = s.node.try_as<flat_check>())
        fn(k->body);
    else if (auto const* const c = s.node.try_as<flat_case>())
    {
        if (is_known(e, c->arms))
            for (auto const& arm : e.at(c->arms))
                fn(arm.body);
        fn(c->default_body);
    }
    else if (auto const* const sw = s.node.try_as<flat_switch>())
    {
        if (is_known(e, sw->arms))
            for (auto const& arm : e.at(sw->arms))
                fn(arm.body);
        fn(sw->default_body);
    }
}

/// Finds the first node of a flat tree that stands deeper than `k_max_program_depth`, counted the way every later walk
/// counts.
/// It descends at most one level past the limit, so it is safe on any tree the flattener wrote.
struct depth_probe
{
    flat_entry_point const& e;
    cc::optional<origin> found;

    void expr(flat_expr_id id, int depth)
    {
        if (found.has_value() || !is_known(e, id))
            return;
        auto const& x = e.at(id);
        if (depth > k_max_program_depth)
        {
            found = x.from;
            return;
        }
        if (auto const* const b = x.node.try_as<flat_block>())
            body(b->body, depth + 1);
        for_each_operand(e, x, [&](flat_expr_id operand) { expr(operand, depth + 1); });
    }

    void body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (found.has_value() || !is_known(e, range))
            return;
        for (auto const id : e.at(range))
        {
            if (found.has_value() || !is_known(e, id))
                continue;
            auto const& s = e.at(id);
            if (depth > k_max_program_depth)
            {
                found = s.from;
                return;
            }
            for_each_expr_of(e, s, [&](flat_expr_id x) { expr(x, depth + 1); });
            for_each_pattern_of(e, s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e, patterns))
                                        for (auto const p : e.at(patterns))
                                            expr(p, depth + 1);
                                });
            for_each_body_of(e, s, [&](ast::range_of<flat_stmt_id> inner) { body(inner, depth + 1); });
        }
    }
};
} // namespace sgl::check::impl
