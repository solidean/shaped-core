#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// How many `leave $label` the statements of `body` hold, at any depth and inside block expressions too.
struct leave_counter
{
    flat_entry_point const& e;
    label_id label;
    int count = 0;

    void expr(flat_expr_id id, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth)
            return;
        auto const& x = e.at(id);
        if (auto const* const b = x.node.try_as<flat_block>())
            body(b->body, depth + 1);
        for_each_operand(e, x, [&](flat_expr_id operand) { expr(operand, depth + 1); });
    }

    void body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e, range) || depth > k_max_depth)
            return;
        for (auto const id : e.at(range))
        {
            if (!is_known(e, id))
                continue;
            auto const& s = e.at(id);
            if (auto const* const l = s.node.try_as<flat_leave>(); l != nullptr && l->target == label)
                ++count;
            for_each_expr_of(s, [&](flat_expr_id x) { expr(x, depth + 1); });
            for_each_body_of(e, s, [&](ast::range_of<flat_stmt_id> inner) { body(inner, depth + 1); });
        }
    }
};

/// The binding member a buffer element reads or writes; `none` where the buffer is named any other way.
struct buffer_ref
{
    symbol_id binding = symbol_id::none;
    i32 member = -1;

    constexpr bool operator==(buffer_ref const&) const = default;
};

buffer_ref buffer_of(flat_entry_point const& e, flat_buffer_element const& element)
{
    if (!is_known(e, element.buffer))
        return {};
    auto const* const b = e.at(element.buffer).node.try_as<flat_binding_member>();
    if (b == nullptr)
        return {};
    return {.binding = b->binding, .member = b->member};
}

/// The locals and the buffers the statements of a list assign, at any depth; by then no expression holds a statement.
struct assigned_locals
{
    flat_entry_point const& e;
    cc::vector<local_id> locals;
    cc::vector<buffer_ref> buffers;

    void place(flat_expr_id id)
    {
        for (auto depth = 0; is_known(e, id) && depth < k_max_depth; ++depth)
        {
            auto const& x = e.at(id);
            if (auto const* const ref = x.node.try_as<flat_local_ref>())
            {
                locals.push_back(ref->local);
                return;
            }
            if (auto const* const element = x.node.try_as<flat_buffer_element>())
            {
                buffers.push_back(buffer_of(e, *element));
                return;
            }
            auto const* const member = x.node.try_as<flat_member>();
            if (member == nullptr)
                return;
            id = member->object;
        }
    }

    void stmt(flat_stmt_id id, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth)
            return;
        auto const& s = e.at(id);
        if (auto const* const a = s.node.try_as<flat_assign>())
            place(a->place);
        for_each_body_of(e, s,
                         [&](ast::range_of<flat_stmt_id> inner)
                         {
                             if (is_known(e, inner))
                                 for (auto const child : e.at(inner))
                                     stmt(child, depth + 1);
                         });
    }
};

/// True when `id` reads one of the locals `assigned` holds, or an element of one of its buffers.
bool reads_any(flat_entry_point const& e, flat_expr_id id, assigned_locals const& assigned, int depth = 0)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return true;
    auto const& x = e.at(id);
    if (auto const* const ref = x.node.try_as<flat_local_ref>())
    {
        for (auto const l : assigned.locals)
            if (l == ref->local)
                return true;
        return false;
    }
    if (auto const* const element = x.node.try_as<flat_buffer_element>())
        for (auto const b : assigned.buffers)
            if (b == buffer_of(e, *element))
                return true;
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || reads_any(e, operand, assigned, depth + 1); });
    return result;
}

bool reads_mutable(flat_entry_point const& e, flat_expr_id id, int depth = 0)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return true;
    auto const& x = e.at(id);
    if (auto const* const ref = x.node.try_as<flat_local_ref>())
        return !is_known(e, ref->local) || e.at(ref->local).is_mut;
    // an element may be stored to between two reads of it
    if (x.node.is<flat_buffer_element>())
        return true;
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || reads_mutable(e, operand, depth + 1); });
    return result;
}

struct expr_lowering
{
    flat_builder& out;
    legalize_options const& options;
    /// Parallel to the labels: the `var` a block expression's leaves assign, `none` for every other label.
    cc::vector<local_id> result_of;
    /// The labels of the enclosing `once`s and loops, which is what a `break` of the input names.
    cc::vector<label_id> breakables;
    cc::vector<label_id> loops;
    int depth = 0;

    local_id result_of_label(label_id label) const
    {
        return is_valid(label) && index_of(label) < result_of.size() ? result_of[index_of(label)] : local_id::none;
    }

    stmt_list ids_of(ast::range_of<flat_stmt_id> range) const
    {
        auto result = stmt_list();
        if (is_known(out.e, range))
            result.push_back_range(out.e.at(range));
        return result;
    }

    cc::string_view label_name(label_id label, cc::string_view fallback) const
    {
        return is_known(out.e, label) ? cc::string_view(out.e.at(label).name) : fallback;
    }

    /// What a pin of `id` is called: the local, the member or the callee it holds, and that it is the value from before.
    cc::string pin_name(flat_expr_id id) const
    {
        auto const& x = out.e.at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>(); ref != nullptr && is_known(out.e, ref->local))
            return cc::format("{}_before", out.e.at(ref->local).name);
        if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && is_valid(call->callee))
            return cc::format("{}_value", out.m.at(call->callee).name);
        if (auto const* const member = x.node.try_as<flat_member>(); member != nullptr && is_known(out.e, member->object))
        {
            auto const fields = out.m.at(out.m.at(out.e.at(member->object).type).members);
            if (member->member >= 0 && member->member < fields.size())
                return cc::format("{}_before", fields[member->member].name);
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
            if (auto const b = buffer_of(out.e, *element); is_valid(b.binding))
            {
                auto const members = out.m.at(out.m.bindings[out.m.at(b.binding).info].members);
                if (b.member >= 0 && b.member < members.size())
                    return cc::format("{}_before", members[b.member].name);
            }
        return "operand";
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    /// Rule E2 over the operands of one node.
    /// `pre` is the list the statement that holds them will join, so whatever lands there runs before it.
    cc::vector<flat_expr_id> lower_operands(cc::span<flat_expr_id const> operands, stmt_list& pre)
    {
        auto result = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < operands.size(); ++i)
        {
            auto const mark = pre.size();
            result.push_back(lower_expr(operands[i], pre));
            if (pre.size() == mark || options.skip_pinning)
                continue;

            auto moved = assigned_locals{.e = out.e};
            for (auto k = mark; k < pre.size(); ++k)
                moved.stmt(pre[k], 0);

            auto at = mark;
            for (auto j = isize(0); j < i; ++j)
            {
                auto const operand = result[j];
                if (!is_known(out.e, operand))
                    continue;
                if (!has_effect(out.e, operand) && !reads_any(out.e, operand, moved))
                    continue;
                out.from = out.e.at(operand).from;
                auto const pin = out.let(pin_name(operand), operand);
                out.e.locals[index_of(pin.local)].kind = local_kind::temporary;
                pre.insert_at(at++, pin.stmt);
                result[j] = out.local(pin.local);
            }
        }
        return result;
    }

    /// Rule E1.
    flat_expr_id lower_block(flat_expr const& x, flat_block const& block, stmt_list& pre)
    {
        auto const body = ids_of(block.body);

        auto counter = leave_counter{.e = out.e, .label = block.label};
        counter.body(block.body, 0);
        auto const* const last
            = body.empty() || !is_known(out.e, body.back()) ? nullptr : out.e.at(body.back()).node.try_as<flat_leave>();
        if (counter.count == 1 && last != nullptr && last->target == block.label && is_valid(last->value))
        {
            // One exit, and it is the end: the statements move and the value stays where the block stood.
            auto const value = last->value;
            for (auto i = isize(0); i + 1 < body.size(); ++i)
                lower_stmt(body[i], pre);
            return lower_expr(value, pre);
        }

        out.from = x.from;
        auto const result = add_temporary_var(out, cc::format("{}_result", label_name(block.label, "block")), x.type);
        if (is_valid(block.label) && index_of(block.label) < result_of.size())
            result_of[index_of(block.label)] = result;
        pre.push_back(out.var(result));
        auto const inner = lower_body(body);
        out.from = x.from;
        pre.push_back(out.block(block.label, inner));
        return out.local(result);
    }

    /// Rule E3.
    flat_expr_id lower_short_circuit(flat_expr_id id,
                                     flat_expr const& x,
                                     flat_expr_id lhs,
                                     flat_expr_id rhs,
                                     bool is_and,
                                     stmt_list& pre)
    {
        auto const new_lhs = lower_expr(lhs, pre);
        auto rhs_pre = stmt_list();
        auto const new_rhs = lower_expr(rhs, rhs_pre);
        if (rhs_pre.empty() && !has_effect(out.e, new_rhs))
        {
            if (new_lhs == lhs && new_rhs == rhs)
                return id;
            out.from = x.from;
            return is_and ? out.add_expr(x.type, flat_and{.lhs = new_lhs, .rhs = new_rhs})
                          : out.add_expr(x.type, flat_or{.lhs = new_lhs, .rhs = new_rhs});
        }

        out.from = x.from;
        auto const result = add_temporary_var(out, is_and ? "and_result" : "or_result", x.type);
        pre.push_back(out.var(result, new_lhs));
        rhs_pre.push_back(out.assign(out.local(result), new_rhs));
        auto const condition = is_and ? out.local(result) : negated(out, out.local(result));
        pre.push_back(out.if_(condition, rhs_pre));
        return out.local(result);
    }

    flat_expr_id lower_expr(flat_expr_id id, stmt_list& pre)
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
            return id;
        ++depth;
        auto const result = lower_expr_unguarded(id, pre);
        --depth;
        return result;
    }

    flat_expr_id lower_expr_unguarded(flat_expr_id id, stmt_list& pre)
    {
        auto const x = out.e.at(id);
        if (auto const* const block = x.node.try_as<flat_block>())
            return lower_block(x, *block, pre);
        if (auto const* const a = x.node.try_as<flat_and>())
            return lower_short_circuit(id, x, a->lhs, a->rhs, true, pre);
        if (auto const* const o = x.node.try_as<flat_or>())
            return lower_short_circuit(id, x, o->lhs, o->rhs, false, pre);

        auto operands = cc::vector<flat_expr_id>();
        for_each_operand(out.e, x, [&](flat_expr_id operand) { operands.push_back(operand); });
        if (operands.empty())
            return id;
        auto const lowered = lower_operands(operands, pre);
        auto is_same = true;
        for (auto i = isize(0); i < operands.size(); ++i)
            is_same = is_same && operands[i] == lowered[i];
        if (is_same)
            return id;

        out.from = x.from;
        auto copy = x;
        if (auto* const member = copy.node.try_as<flat_member>())
            member->object = lowered[0];
        else if (auto* const element = copy.node.try_as<flat_buffer_element>())
        {
            element->buffer = lowered[0];
            element->index = lowered[1];
        }
        else if (auto* const construct = copy.node.try_as<flat_construct>())
            construct->arguments = out.expr_list(lowered);
        else if (auto* const call = copy.node.try_as<flat_call>())
            call->arguments = out.expr_list(lowered);
        else if (auto* const n = copy.node.try_as<flat_not>())
            n->operand = lowered[0];
        out.e.exprs.push_back(cc::move(copy));
        return flat_expr_id(out.e.exprs.size() - 1);
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    stmt_list lower_body(stmt_list const& body)
    {
        auto result = stmt_list();
        for (auto const id : body)
            lower_stmt(id, result);
        return result;
    }

    stmt_list lower_body(ast::range_of<flat_stmt_id> range) { return lower_body(ids_of(range)); }

    label_id label_or_new(label_id label, cc::string_view desired)
    {
        if (is_known(out.e, label))
            return label;
        auto const fresh = out.add_label(desired);
        result_of.push_back(local_id::none);
        return fresh;
    }

    stmt_list lower_loop_body(label_id label, ast::range_of<flat_stmt_id> body)
    {
        breakables.push_back(label);
        loops.push_back(label);
        auto result = lower_body(body);
        loops.remove_back();
        breakables.remove_back();
        return result;
    }

    void lower_stmt(flat_stmt_id id, stmt_list& into)
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
        {
            into.push_back(id);
            return;
        }
        ++depth;
        lower_stmt_unguarded(id, into);
        --depth;
    }

    void lower_stmt_unguarded(flat_stmt_id id, stmt_list& into)
    {
        auto const s = out.e.at(id);
        auto const from = s.from;
        auto const attributed = [&]() -> flat_builder&
        {
            out.from = from;
            out.inlined_through = s.inlined_through;
            return out;
        };

        if (auto const* const let = s.node.try_as<flat_let>())
        {
            auto const value = lower_expr(let->value, into);
            into.push_back(attributed().let(let->local, value));
        }
        else if (auto const* const var = s.node.try_as<flat_var>())
        {
            auto const value = is_valid(var->value) ? lower_expr(var->value, into) : var->value;
            into.push_back(attributed().var(var->local, value));
        }
        else if (auto const* const assign = s.node.try_as<flat_assign>())
            lower_assign(*assign, attributed, into);
        else if (auto const* const print = s.node.try_as<flat_print>())
        {
            auto const value = lower_expr(print->value, into);
            into.push_back(attributed().print(value));
        }
        else if (auto const* const eval = s.node.try_as<flat_eval>())
        {
            // A block that was the value has moved in front, and what is left of it is the local that holds its result.
            // Reading a local or a literal is nothing to evaluate, so such a rest is dropped; a call stays, pure or not.
            auto const value = lower_expr(eval->value, into);
            auto const is_leaf
                = is_known(out.e, value)
               && (out.e.at(value).node.is<flat_local_ref>() || out.e.at(value).node.is<flat_literal>()
                   || out.e.at(value).node.is<flat_int_literal>() || out.e.at(value).node.is<flat_bool_literal>());
            if (!is_leaf || value == eval->value)
                into.push_back(attributed().eval(value));
        }
        else if (auto const* const branch = s.node.try_as<flat_if>())
        {
            auto const condition = lower_expr(branch->condition, into);
            auto const then_body = lower_body(branch->then_body);
            auto const else_body = lower_body(branch->else_body);
            into.push_back(attributed().if_(condition, then_body, else_body));
        }
        else if (auto const* const block = s.node.try_as<flat_block>())
        {
            auto const body = lower_body(block->body);
            into.push_back(attributed().block(block->label, body));
        }
        else if (auto const* const leave = s.node.try_as<flat_leave>())
        {
            auto const value = is_valid(leave->value) ? lower_expr(leave->value, into) : leave->value;
            auto const result = result_of_label(leave->target);
            if (is_valid(result) && is_valid(value))
            {
                into.push_back(attributed().assign(out.local(result), value));
                into.push_back(out.leave(leave->target));
            }
            else
                into.push_back(attributed().leave(leave->target, value));
        }
        else if (auto const* const loop = s.node.try_as<flat_loop>())
        {
            auto const label = label_or_new(loop->label, "loop");
            auto const body = lower_loop_body(label, loop->body);
            into.push_back(attributed().loop(label, body));
        }
        else if (auto const* const w = s.node.try_as<flat_while>())
            lower_while(*w, from, into);
        else if (auto const* const f = s.node.try_as<flat_for>())
            lower_for(*f, from, into);
        else if (auto const* const c = s.node.try_as<flat_continue>())
        {
            auto const target = is_known(out.e, c->target) || loops.empty() ? c->target : loops.back();
            into.push_back(attributed().continue_(target));
        }
        else if (auto const* const once = s.node.try_as<flat_once>())
        {
            auto const label = label_or_new(label_id::none, "once");
            breakables.push_back(label);
            auto const body = lower_body(once->body);
            breakables.remove_back();
            into.push_back(attributed().block(label, body));
        }
        else if (s.node.is<flat_break>())
        {
            if (breakables.empty())
                into.push_back(id);
            else
                into.push_back(attributed().leave(breakables.back()));
        }
        else if (auto const* const r = s.node.try_as<flat_return>())
        {
            auto const value = lower_expr(r->value, into);
            into.push_back(attributed().return_(value));
        }
        else if (auto const* const sw = s.node.try_as<flat_switch>())
        {
            // Written by C1 behind the first pass of these rules, so its scrutinee and its patterns are core already.
            auto copy = *sw;
            auto original = cc::vector<flat_arm>();
            original.push_back_range(out.e.at(sw->arms));
            auto arms = cc::vector<flat_arm>();
            for (auto const& a : original)
                arms.push_back({.patterns = a.patterns, .body = out.stmt_list(lower_body(a.body))});
            copy.arms = out.arm_list(arms);
            copy.default_body = out.stmt_list(lower_body(sw->default_body));
            into.push_back(attributed().add_stmt(cc::move(copy)));
        }
        else if (auto const* const c = s.node.try_as<flat_case>())
        {
            auto copy = *c;
            copy.scrutinee = lower_expr(c->scrutinee, into);
            // The arms are copied first: lowering a body may write arms of its own, which moves the array.
            auto original = cc::vector<flat_arm>();
            original.push_back_range(out.e.at(c->arms));
            auto arms = cc::vector<flat_arm>();
            for (auto const& a : original)
                arms.push_back({.patterns = a.patterns, .body = out.stmt_list(lower_body(a.body))});
            copy.arms = out.arm_list(arms);
            copy.default_body = out.stmt_list(lower_body(c->default_body));
            into.push_back(attributed().add_stmt(cc::move(copy)));
        }
        else
        {
            // A statement this pass does not lower goes through untouched; `find_core_violation` is what judges it.
            // Dropping it instead would write a shader that quietly does less than its source says.
            into.push_back(id);
        }
    }

    /// EVAL-14: a buffer element's index is evaluated before the value that is stored to it.
    /// What evaluating the value moves in front runs after the index, so an index it could change is pinned first.
    template <class Attributed>
    void lower_assign(flat_assign const& assign, Attributed&& attributed, stmt_list& into)
    {
        auto place = assign.place;
        auto const* const element = is_known(out.e, place) ? out.e.at(place).node.try_as<flat_buffer_element>() : nullptr;
        if (element == nullptr)
        {
            auto const value = lower_expr(assign.value, into);
            into.push_back(attributed().assign(place, value));
            return;
        }

        // by value: lowering appends to the tree
        auto const x = out.e.at(place);
        auto const written = *element;
        auto index = lower_expr(written.index, into);
        auto value_pre = stmt_list();
        auto const value = lower_expr(assign.value, value_pre);
        if (!value_pre.empty() && !options.skip_pinning && is_known(out.e, index))
        {
            auto moved = assigned_locals{.e = out.e};
            for (auto const id : value_pre)
                moved.stmt(id, 0);
            if (has_effect(out.e, index) || reads_any(out.e, index, moved))
            {
                out.from = out.e.at(index).from;
                auto const pin = out.let("index", index);
                out.e.locals[index_of(pin.local)].kind = local_kind::temporary;
                into.push_back(pin.stmt);
                index = out.local(pin.local);
            }
        }
        if (index != written.index)
        {
            out.from = x.from;
            place = out.add_expr(x.type, flat_buffer_element{.buffer = written.buffer, .index = index});
        }
        into.push_back_range(value_pre);
        into.push_back(attributed().assign(place, value));
    }

    /// Rule E4.
    void lower_while(flat_while const& w, origin from, stmt_list& into)
    {
        auto const label = label_or_new(w.label, "while");
        auto condition_pre = stmt_list();
        auto const condition = lower_expr(w.condition, condition_pre);
        auto body = lower_loop_body(label, w.body);
        out.from = from;
        if (condition_pre.empty())
        {
            into.push_back(out.while_(label, condition, body));
            return;
        }

        // At the top of the body, so a `continue` still meets the condition before the next iteration.
        auto const exit = out.leave(label);
        condition_pre.push_back(out.if_(negated(out, condition), {exit}));
        condition_pre.push_back_range(body);
        into.push_back(out.loop(label, condition_pre));
    }

    void lower_for(flat_for const& f, origin from, stmt_list& into)
    {
        auto const label = label_or_new(f.label, "for");
        flat_expr_id const bounds[] = {f.first, f.end};
        auto lowered = lower_operands(bounds, into);

        // A target evaluates the end before every iteration, so it must give the same value every time.
        if (is_known(out.e, lowered[1]) && (has_effect(out.e, lowered[1]) || reads_mutable(out.e, lowered[1])))
        {
            out.from = from;
            if (is_known(out.e, lowered[0]) && has_effect(out.e, lowered[0]))
            {
                auto const first = out.let("first", lowered[0]);
                out.e.locals[index_of(first.local)].kind = local_kind::temporary;
                into.push_back(first.stmt);
                lowered[0] = out.local(first.local);
            }
            auto const name = is_known(out.e, f.index) ? cc::format("{}_end", out.e.at(f.index).name) : cc::string("end");
            auto const end = out.let(name, lowered[1]);
            out.e.locals[index_of(end.local)].kind = local_kind::temporary;
            into.push_back(end.stmt);
            lowered[1] = out.local(end.local);
        }

        auto const body = lower_loop_body(label, f.body);
        out.from = from;
        into.push_back(out.for_(label, f.index, lowered[0], lowered[1], body));
    }
};
} // namespace

local_id sgl::check::impl::add_temporary_var(flat_builder& out, cc::string_view desired, type_id type)
{
    auto const id = out.add_local(local_kind::temporary, desired, type);
    out.e.locals[index_of(id)].is_mut = true;
    return id;
}

flat_expr_id sgl::check::impl::negated(flat_builder& out, flat_expr_id x)
{
    if (is_known(out.e, x))
        if (auto const* const n = out.e.at(x).node.try_as<flat_not>())
            return n->operand;
    auto const type = is_known(out.e, x) ? out.e.at(x).type : type_id::none;
    return out.add_expr(type, flat_not{.operand = x});
}

stmt_list sgl::check::impl::lower_expressions(flat_builder& out, legalize_options const& options)
{
    auto lowering = expr_lowering{.out = out, .options = options};
    lowering.result_of.resize_to_filled(out.e.labels.size(), local_id::none);
    return lowering.lower_body(out.e.body);
}
