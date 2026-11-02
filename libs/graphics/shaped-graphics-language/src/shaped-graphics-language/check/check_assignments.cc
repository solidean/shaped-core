#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// CHK-55: a `let mut` declared without a value is read only where every path to the read assigned it.
// The walk is over the structured flat tree, where every call is inlined, so a helper's body counts where it is called.

namespace
{
/// Which of the tracked locals every path to here has assigned; an unreachable state joins as no constraint.
struct assignment_state
{
    cc::vector<bool> is_set;
    bool is_reachable = true;

    void join(assignment_state const& other)
    {
        if (!other.is_reachable)
            return;
        if (!is_reachable)
        {
            *this = other;
            return;
        }
        for (auto i = isize(0); i < is_set.size(); ++i)
            is_set[i] = is_set[i] && other.is_set[i];
    }
};

struct assignment_pass
{
    flat_entry_point const& e;
    /// Parallel to `e.locals`: declared by a `flat_var` without a value.
    cc::vector<bool> is_tracked;
    /// Parallel to `e.labels`: the join of every `leave` to that label met so far.
    cc::vector<assignment_state> leaving;
    /// The first unassigned read of each tracked local, which is the one reported.
    cc::vector<flat_expr_id> first_read;

    [[nodiscard]] assignment_state unreachable() const
    {
        return {.is_set = cc::vector<bool>::create_filled(e.locals.size(), true), .is_reachable = false};
    }

    void read(flat_expr_id id, assignment_state& state, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth)
            return;
        auto const& x = e.at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
        {
            if (is_known(e, ref->local) && is_tracked[index_of(ref->local)] && state.is_reachable
                && !state.is_set[index_of(ref->local)] && !is_valid(first_read[index_of(ref->local)]))
                first_read[index_of(ref->local)] = id;
            return;
        }
        // a block expression runs its statements where it stands, and `and` / `or` may skip their right side
        if (auto const* const block = x.node.try_as<flat_block>())
        {
            auto inner = walk_list(block->body, state, depth + 1);
            if (is_known(e, block->label))
                inner.join(leaving[index_of(block->label)]);
            state = inner;
            return;
        }
        if (auto const* const both = x.node.try_as<flat_and>())
        {
            read(both->lhs, state, depth + 1);
            auto right = state;
            read(both->rhs, right, depth + 1);
            return;
        }
        if (auto const* const either = x.node.try_as<flat_or>())
        {
            read(either->lhs, state, depth + 1);
            auto right = state;
            read(either->rhs, right, depth + 1);
            return;
        }
        if (auto const* const by_target = x.node.try_as<flat_by_target>())
        {
            auto native = state;
            read(by_target->native, native, depth + 1);
            read(by_target->emulated, state, depth + 1);
            state.join(native);
            return;
        }
        for_each_operand(e, x, [&](flat_expr_id operand) { read(operand, state, depth + 1); });
    }

    /// A place's indices and its object are read, except the local an assignment stores whole.
    void write(flat_expr_id place, assignment_state& state, int depth)
    {
        if (!is_known(e, place))
            return;
        if (auto const* const ref = e.at(place).node.try_as<flat_local_ref>(); ref != nullptr && is_known(e, ref->local))
        {
            state.is_set[index_of(ref->local)] = true;
            return;
        }
        // a member or an element of a local stores part of it, which needs the rest assignment_state already
        read(place, state, depth + 1);
    }

    assignment_state walk_list(ast::range_of<flat_stmt_id> list, assignment_state state, int depth)
    {
        if (!is_known(e, list) || depth > k_max_depth)
            return state;
        for (auto const id : e.at(list))
            walk(id, state, depth + 1);
        return state;
    }

    void leave_to(label_id target, assignment_state const& state)
    {
        if (is_known(e, target))
            leaving[index_of(target)].join(state);
    }

    void walk(flat_stmt_id id, assignment_state& state, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth)
            return;
        auto const& s = e.at(id);
        if (auto const* const var = s.node.try_as<flat_var>())
        {
            if (is_valid(var->value))
                read(var->value, state, depth);
            if (is_known(e, var->local))
                state.is_set[index_of(var->local)] = is_valid(var->value);
        }
        else if (auto const* const assign = s.node.try_as<flat_assign>())
        {
            read(assign->value, state, depth);
            write(assign->place, state, depth);
        }
        else if (auto const* const branch = s.node.try_as<flat_if>())
        {
            read(branch->condition, state, depth);
            auto then_state = walk_list(branch->then_body, state, depth);
            auto else_state = walk_list(branch->else_body, state, depth);
            then_state.join(else_state);
            state = then_state;
        }
        else if (auto const* const block = s.node.try_as<flat_block>())
        {
            auto after = walk_list(block->body, state, depth);
            if (is_known(e, block->label))
                after.join(leaving[index_of(block->label)]);
            state = after;
        }
        else if (auto const* const leave = s.node.try_as<flat_leave>())
        {
            read(leave->value, state, depth);
            leave_to(leave->target, state);
            state = unreachable();
        }
        else if (auto const* const loop = s.node.try_as<flat_loop>())
        {
            // it runs until something leaves it, so what follows is reached through those leaves alone
            (void)walk_list(loop->body, state, depth);
            state = is_known(e, loop->label) ? leaving[index_of(loop->label)] : unreachable();
        }
        else if (auto const* const check = s.node.try_as<flat_check>())
        {
            // its condition is the statements of its body, which run where it stands
            state = walk_list(check->body, state, depth);
        }
        else if (auto const* const once = s.node.try_as<flat_once>())
        {
            state = walk_list(once->body, state, depth);
        }
        else if (auto const* const loop = s.node.try_as<flat_while>())
        {
            // the body may never run, so nothing it assigns counts after it
            read(loop->condition, state, depth);
            (void)walk_list(loop->body, state, depth);
        }
        else if (auto const* const loop = s.node.try_as<flat_for>())
        {
            read(loop->first, state, depth);
            read(loop->end, state, depth);
            (void)walk_list(loop->body, state, depth);
        }
        else if (auto const* const c = s.node.try_as<flat_case>())
        {
            read(c->scrutinee, state, depth);
            auto after = walk_list(c->default_body, state, depth);
            if (is_known(e, c->arms))
                for (auto const& arm : e.at(c->arms))
                    after.join(walk_list(arm.body, state, depth));
            state = after;
        }
        else if (auto const* const sw = s.node.try_as<flat_switch>())
        {
            read(sw->scrutinee, state, depth);
            auto after = walk_list(sw->default_body, state, depth);
            if (is_known(e, sw->arms))
                for (auto const& arm : e.at(sw->arms))
                    after.join(walk_list(arm.body, state, depth));
            state = after;
        }
        else if (s.node.is<flat_return>() || s.node.is<flat_discard>() || s.node.is<flat_continue>()
                 || s.node.is<flat_break>())
        {
            for_each_expr_of(s, [&](flat_expr_id x) { read(x, state, depth); });
            state = unreachable();
        }
        else
            for_each_expr_of(s, [&](flat_expr_id x) { read(x, state, depth); });
    }
};
} // namespace

void checker::judge_assignments(flat_entry_point const& structured)
{
    auto pass = assignment_pass{
        .e = structured,
        .is_tracked = cc::vector<bool>::create_filled(structured.locals.size(), false),
        .leaving = {},
        .first_read = cc::vector<flat_expr_id>::create_filled(structured.locals.size(), flat_expr_id::none)};
    auto any = false;
    // only what the program declared so: a `var` the flattener writes for its own lowering is assignment_state by it
    auto const is_declared_without_value = [&](flat_stmt const& s)
    {
        if (!ast::is_valid(s.from.stmt))
            return false;
        auto const* const let = ast_of(s.from.file).at(s.from.stmt).node.try_as<ast::let_stmt>();
        return let != nullptr && !ast::is_valid(let->value);
    };
    for (auto const& s : structured.stmts)
        if (auto const* const var = s.node.try_as<flat_var>();
            var != nullptr && !is_valid(var->value) && is_known(structured, var->local) && is_declared_without_value(s))
        {
            pass.is_tracked[index_of(var->local)] = true;
            any = true;
        }
    if (!any)
        return;
    pass.leaving = cc::vector<assignment_state>::create_filled(structured.labels.size(), pass.unreachable());
    (void)pass.walk_list(structured.body,
                         assignment_state{.is_set = cc::vector<bool>::create_filled(structured.locals.size(), true)}, 0);

    for (auto i = isize(0); i < structured.locals.size(); ++i)
    {
        auto const read = pass.first_read[i];
        if (!is_valid(read))
            continue;
        auto const& from = structured.at(read).from;
        if (!ast::is_valid(from.expr))
            continue;
        report_once(diagnostic_kind::read_before_assignment, from.file, span_of(from.file, from.expr),
                    cc::format("{} is read here, and a path to here assigns it nothing; give it a value where it is "
                               "declared, or assign it on every path first",
                               structured.locals[i].name));
    }
}
