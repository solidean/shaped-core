#include <clean-core/common/utility.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Rule C1: a `case` is a `switch` where every pattern is a literal, and a chain of `if` over `==` otherwise.
// It runs in front of the expression rules, so what it writes is an ordinary tree they already know.

namespace
{
stmt_list ids_of(flat_entry_point const& e, ast::range_of<flat_stmt_id> range)
{
    auto result = stmt_list();
    if (is_known(e, range))
        result.push_back_range(e.at(range));
    return result;
}

struct case_lowering
{
    flat_builder& out;
    type_id bool_type = type_id::none;

    /// What a target can write as a `switch` label, which is what decides the form (LEGAL-45).
    [[nodiscard]] bool is_literal(flat_expr_id id) const
    {
        if (!is_known(out.e, id))
            return false;
        auto const& x = out.e.at(id);
        return x.node.is<flat_int_literal>() || x.node.is<flat_enum_value>();
    }

    ast::range_of<flat_stmt_id> lower_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        return out.stmt_list(lower(ids_of(out.e, range), depth + 1));
    }

    /// `scrutinee == pattern`, as the record the check pass resolved.
    [[nodiscard]] flat_expr_id equals(flat_case const& c, flat_expr_id lhs, flat_expr_id rhs)
    {
        flat_expr_id const arguments[] = {lhs, rhs};
        return out.add_expr(bool_type, flat_call{.callee = c.equality,
                                                 .intrinsic = c.equality_intrinsic,
                                                 .is_pure = true,
                                                 .arguments = out.expr_list(arguments)});
    }

    void lower_case(flat_stmt const& s, flat_case const& c, stmt_list& into, int depth)
    {
        // Copied first: lowering a body may write arms of its own, which moves the array this span names.
        auto original = cc::vector<flat_arm>();
        original.push_back_range(out.e.at(c.arms));
        auto arms = cc::vector<flat_arm>();
        auto is_all_literal = true;
        for (auto const& a : original)
        {
            if (!is_known(out.e, a.patterns))
                continue;
            auto patterns = cc::vector<flat_expr_id>();
            patterns.push_back_range(out.e.at(a.patterns));
            for (auto const p : patterns)
                is_all_literal = is_all_literal && is_literal(p);
            arms.push_back({.patterns = a.patterns, .body = lower_body(a.body, depth)});
        }
        auto const default_body = lower_body(c.default_body, depth);

        // One `_` and nothing else: the arms are the default, and no construct is needed at all.
        if (arms.empty())
        {
            into.push_back_range(ids_of(out.e, default_body));
            return;
        }

        out.from = s.from;
        out.inlined_through = s.inlined_through;

        if (is_all_literal && is_valid(c.scrutinee))
        {
            into.push_back(out.add_stmt(flat_switch{
                .scrutinee = c.scrutinee,
                .arms = out.arm_list(arms),
                .default_body = default_body,
            }));
            return;
        }

        // C1: the scrutinee is bound once, and each arm becomes an `if` whose condition is its patterns joined by `or`.
        if (!is_valid(c.equality))
        {
            // Nothing resolved an `==`, so the chain cannot be written; the core check refuses what is left.
            into.push_back(out.add_stmt(c));
            return;
        }
        auto const bound = out.let("case_value", c.scrutinee);
        into.push_back(bound.stmt);

        auto chain = ids_of(out.e, default_body);
        for (auto i = arms.size() - 1; i >= 0; --i)
        {
            // Copied first: writing a condition appends to the expression lists, which moves this span.
            auto patterns = cc::vector<flat_expr_id>();
            patterns.push_back_range(out.e.at(arms[i].patterns));

            auto condition = flat_expr_id::none;
            for (auto const p : patterns)
            {
                out.from = s.from;
                auto const one = equals(c, out.local(bound.local), p);
                condition = is_valid(condition) ? out.or_(condition, one) : one;
            }
            out.from = s.from;
            auto const then_body = ids_of(out.e, arms[i].body);
            auto const written = out.if_(condition, then_body, chain);
            chain = stmt_list();
            chain.push_back(written);
        }
        into.push_back_range(chain);
    }

    stmt_list lower(stmt_list const& list, int depth)
    {
        auto result = stmt_list();
        for (auto const id : list)
        {
            if (!is_known(out.e, id) || depth > k_max_depth)
            {
                result.push_back(id);
                continue;
            }
            auto const s = out.e.at(id);
            out.from = s.from;
            out.inlined_through = s.inlined_through;

            if (auto const* const c = s.node.try_as<flat_case>())
            {
                lower_case(s, *c, result, depth);
                continue;
            }
            if (auto const* const branch = s.node.try_as<flat_if>())
            {
                auto const then_body = lower_body(branch->then_body, depth);
                auto const else_body = lower_body(branch->else_body, depth);
                out.from = s.from;
                auto copy = *branch;
                copy.then_body = then_body;
                copy.else_body = else_body;
                result.push_back(out.add_stmt(copy));
                continue;
            }
            // Every other body-holding statement keeps its shape, with its lists lowered.
            auto copy = s;
            auto is_rewritten = false;
            copy.node.visit(
                [&](flat_block& n)
                {
                    n.body = lower_body(n.body, depth);
                    is_rewritten = true;
                },
                [&](flat_loop& n)
                {
                    n.body = lower_body(n.body, depth);
                    is_rewritten = true;
                },
                [&](flat_while& n)
                {
                    n.body = lower_body(n.body, depth);
                    is_rewritten = true;
                },
                [&](flat_for& n)
                {
                    n.body = lower_body(n.body, depth);
                    is_rewritten = true;
                },
                [&](flat_once& n)
                {
                    n.body = lower_body(n.body, depth);
                    is_rewritten = true;
                },
                [&](flat_switch& n)
                {
                    auto original = cc::vector<flat_arm>();
                    original.push_back_range(out.e.at(n.arms));
                    auto arms = cc::vector<flat_arm>();
                    for (auto const& a : original)
                        arms.push_back({.patterns = a.patterns, .body = lower_body(a.body, depth)});
                    n.arms = out.arm_list(arms);
                    n.default_body = lower_body(n.default_body, depth);
                    is_rewritten = true;
                },
                [&](flat_let&) {}, [&](flat_var&) {}, [&](flat_assign&) {}, [&](flat_print&) {}, [&](flat_eval&) {},
                [&](flat_leave&) {}, [&](flat_continue&) {}, [&](flat_break&) {}, [&](flat_case&) {}, [&](flat_if&) {},
                [&](flat_return&) {});
            if (!is_rewritten)
            {
                result.push_back(id);
                continue;
            }
            out.from = s.from;
            out.inlined_through = s.inlined_through;
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return result;
    }
};
} // namespace

stmt_list sgl::check::impl::lower_cases(flat_builder& out)
{
    auto lowering = case_lowering{.out = out, .bool_type = out.type_named(builtins::k_bool)};
    return lowering.lower(ids_of(out.e, out.e.body), 0);
}
