#include "core.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

/// True when `id` has an effect, or cannot be walked.
bool has_effect_at(flat_entry_point const& e, flat_expr_id id, int depth)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return true;
    auto const& x = e.at(id);
    if (x.node.is<flat_block>() || x.node.is<flat_invalid>())
        return true;
    if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
        return true;
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || has_effect_at(e, operand, depth + 1); });
    return result;
}

bool reads_mutable_local(flat_entry_point const& e, flat_expr_id id, int depth)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return true;
    auto const& x = e.at(id);
    if (auto const* const ref = x.node.try_as<flat_local_ref>())
        return !is_known(e, ref->local) || e.at(ref->local).is_mut;
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || reads_mutable_local(e, operand, depth + 1); });
    return result;
}

struct core_checker
{
    flat_entry_point const& e;
    cc::optional<core_violation> found;

    struct breakable
    {
        bool is_loop = false;
        label_id label = label_id::none;
    };
    cc::vector<breakable> enclosing;
    flat_stmt_id current = flat_stmt_id::none;

    void violation(cc::string reason, flat_expr_id expr = flat_expr_id::none)
    {
        if (!found.has_value())
            found = core_violation{.reason = cc::move(reason), .stmt = current, .expr = expr};
    }

    cc::string_view label_name(label_id id) const
    {
        return is_known(e, id) ? cc::string_view(e.at(id).name) : cc::string_view("?");
    }

    void expr(flat_expr_id id, int depth)
    {
        if (found.has_value())
            return;
        if (!is_known(e, id))
            return violation("an expression id that names nothing", id);
        if (depth > k_max_depth)
            return violation("an expression nested beyond any program", id);

        auto const& x = e.at(id);
        if (auto const* const b = x.node.try_as<flat_block>())
            return violation(
                cc::format("the block expression ${}, since a core expression holds no statement", label_name(b->label)),
                id);
        if (auto const* const a = x.node.try_as<flat_and>(); a != nullptr && has_effect_at(e, a->rhs, depth))
            return violation("an `and` whose right operand has an effect, which must be an `if`", id);
        if (auto const* const o = x.node.try_as<flat_or>(); o != nullptr && has_effect_at(e, o->rhs, depth))
            return violation("an `or` whose right operand has an effect, which must be an `if`", id);
        for_each_operand(e, x, [&](flat_expr_id operand) { expr(operand, depth + 1); });
    }

    void optional_expr(flat_expr_id id)
    {
        if (is_valid(id))
            expr(id, 0);
    }

    void body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e, range))
            return violation("a statement list that reaches outside the tree");
        for (auto const id : e.at(range))
            stmt(id, depth);
    }

    void breakable_body(breakable b, ast::range_of<flat_stmt_id> range, int depth)
    {
        enclosing.push_back(b);
        body(range, depth);
        enclosing.remove_back();
    }

    void stmt(flat_stmt_id id, int depth)
    {
        if (found.has_value())
            return;
        if (!is_known(e, id))
            return violation("a statement id that names nothing");
        if (depth > k_max_depth)
            return violation("a statement nested beyond any program");
        current = id;

        e.at(id).node.visit(
            [&](flat_let const& s) { expr(s.value, 0); }, //
            [&](flat_var const& s) { optional_expr(s.value); },
            [&](flat_assign const& s)
            {
                expr(s.place, 0);
                expr(s.value, 0);
            },
            [&](flat_print const& s) { expr(s.value, 0); },
            [&](flat_if const& s)
            {
                expr(s.condition, 0);
                body(s.then_body, depth + 1);
                current = id;
                body(s.else_body, depth + 1);
            },
            [&](flat_block const& s)
            { violation(cc::format("the block ${}, which must be a once or nothing at all", label_name(s.label))); },
            [&](flat_leave const& s)
            {
                violation(
                    cc::format("a leave of ${}, which must be a break, a continue or a return", label_name(s.target)));
            },
            [&](flat_loop const& s) { breakable_body({.is_loop = true, .label = s.label}, s.body, depth + 1); },
            [&](flat_while const& s)
            {
                expr(s.condition, 0);
                breakable_body({.is_loop = true, .label = s.label}, s.body, depth + 1);
            },
            [&](flat_for const& s)
            {
                expr(s.first, 0);
                expr(s.end, 0);
                if (!found.has_value() && (has_effect_at(e, s.end, 0) || reads_mutable_local(e, s.end, 0)))
                    violation("a `for` whose end has an effect or reads a mutable local, since a target evaluates it "
                              "before every iteration",
                              s.end);
                breakable_body({.is_loop = true, .label = s.label}, s.body, depth + 1);
            },
            [&](flat_continue const& s)
            {
                if (enclosing.empty())
                    violation("a continue outside every loop");
                else if (!enclosing.back().is_loop)
                    violation(cc::format("a continue of ${} that would cross a once", label_name(s.target)));
                else if (enclosing.back().label != s.target)
                    violation(cc::format("a continue of ${}, which is not the innermost loop", label_name(s.target)));
            },
            [&](flat_once const& s) { breakable_body({.is_loop = false}, s.body, depth + 1); },
            [&](flat_break const&)
            {
                if (enclosing.empty())
                    violation("a break outside every once and every loop");
            },
            [&](flat_return const& s) { expr(s.value, 0); });
    }
};
} // namespace

cc::optional<sgl::check::core_violation> sgl::check::find_core_violation(flat_entry_point const& e)
{
    auto c = core_checker{.e = e};
    c.body(e.body, 0);
    return cc::move(c.found);
}

bool sgl::check::is_core(flat_entry_point const& e)
{
    return !find_core_violation(e).has_value();
}

bool sgl::check::has_effect(flat_entry_point const& e, flat_expr_id id)
{
    return has_effect_at(e, id, 0);
}
