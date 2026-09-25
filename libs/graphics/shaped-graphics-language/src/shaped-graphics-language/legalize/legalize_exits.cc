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
stmt_list ids_of(flat_entry_point const& e, ast::range_of<flat_stmt_id> range)
{
    auto result = stmt_list();
    if (is_known(e, range))
        result.push_back_range(e.at(range));
    return result;
}

/// True for a statement after which the rest of its list never runs.
bool is_exit(flat_stmt const& s)
{
    return s.node.is<flat_leave>() || s.node.is<flat_continue>() || s.node.is<flat_break>() || s.node.is<flat_return>();
}

bool holds_leave(flat_entry_point const& e, flat_stmt_id id, label_id label, int depth = 0)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return false;
    auto const& s = e.at(id);
    if (auto const* const l = s.node.try_as<flat_leave>())
        return l->target == label;
    auto result = false;
    for_each_body_of(e, s,
                     [&](ast::range_of<flat_stmt_id> body)
                     {
                         if (is_known(e, body))
                             for (auto const child : e.at(body))
                                 result = result || holds_leave(e, child, label, depth + 1);
                     });
    return result;
}

/// False only when no path through `list` reaches its end; an answer of true may be wrong, and costs a `once`.
bool may_fall_through(flat_entry_point const& e, cc::span<flat_stmt_id const> list, int depth = 0)
{
    if (depth > k_max_depth)
        return true;
    for (auto const id : list)
    {
        if (!is_known(e, id))
            continue;
        auto const& s = e.at(id);
        if (is_exit(s))
            return false;
        if (auto const* const branch = s.node.try_as<flat_if>())
            if (is_known(e, branch->then_body) && is_known(e, branch->else_body)
                && !may_fall_through(e, e.at(branch->then_body), depth + 1)
                && !may_fall_through(e, e.at(branch->else_body), depth + 1))
                return false;
    }
    return true;
}

/// Rule X2, bottom-up: a block whose leaves all stand in tail position becomes its statements.
struct block_dissolver
{
    flat_builder& out;

    /// An `if` that reads well: no empty `then` beside a filled `else`, and nothing at all where nothing happens.
    void push_if(flat_stmt const& original,
                 flat_expr_id condition,
                 stmt_list const& then_body,
                 stmt_list const& else_body,
                 stmt_list& into)
    {
        out.from = original.from;
        out.inlined_through = original.inlined_through;
        if (then_body.empty() && else_body.empty())
        {
            if (has_effect(out.e, condition))
                into.push_back(out.if_(condition, then_body));
            return;
        }
        if (then_body.empty())
            into.push_back(out.if_(negated(out, condition), else_body));
        else
            into.push_back(out.if_(condition, then_body, else_body));
    }

    /// `list` as the end of block `label`, with every `leave $label` gone: reaching the end of the result IS leaving.
    /// False when a leave stands where that cannot be said without writing a statement twice.
    bool tail(stmt_list const& list, label_id label, stmt_list& result, int depth = 0)
    {
        if (depth > k_max_depth)
            return false;
        for (auto i = isize(0); i < list.size(); ++i)
        {
            auto const id = list[i];
            if (!is_known(out.e, id))
            {
                result.push_back(id);
                continue;
            }
            auto const s = out.e.at(id);
            if (auto const* const l = s.node.try_as<flat_leave>(); l != nullptr && l->target == label)
                return true;
            if (!holds_leave(out.e, id, label))
            {
                result.push_back(id);
                if (is_exit(s))
                    return true;
                continue;
            }
            // LEGAL-49: where the switch ends the block, leaving an arm IS leaving the block, so both labels go.
            if (auto const* const sw = s.node.try_as<flat_switch>())
            {
                if (i + 1 < list.size())
                    return false;
                auto original = cc::vector<flat_arm>();
                original.push_back_range(out.e.at(sw->arms));
                auto arms = cc::vector<flat_arm>();
                for (auto const& a : original)
                {
                    auto body = stmt_list();
                    if (!tail(ids_of(out.e, a.body), label, body, depth + 1))
                        return false;
                    arms.push_back({.patterns = a.patterns, .body = out.stmt_list(body)});
                }
                auto default_body = stmt_list();
                if (!tail(ids_of(out.e, sw->default_body), label, default_body, depth + 1))
                    return false;
                out.from = s.from;
                out.inlined_through = s.inlined_through;
                result.push_back(out.add_stmt(flat_switch{.scrutinee = sw->scrutinee,
                                                          .arms = out.arm_list(arms),
                                                          .default_body = out.stmt_list(default_body)}));
                return true;
            }

            auto const* const branch = s.node.try_as<flat_if>();
            if (branch == nullptr)
                return false;

            auto then_body = ids_of(out.e, branch->then_body);
            auto else_body = ids_of(out.e, branch->else_body);
            if (i + 1 < list.size())
            {
                // What follows the `if` runs only on a path that fell through it, so it moves to where that path ends.
                auto const then_falls = may_fall_through(out.e, then_body);
                auto const else_falls = may_fall_through(out.e, else_body);
                if (then_falls && else_falls)
                    return false;
                auto const rest
                    = cc::span<flat_stmt_id const>(list).subspan({.offset = i + 1, .size = list.size() - i - 1});
                if (then_falls)
                    then_body.push_back_range(rest);
                if (else_falls)
                    else_body.push_back_range(rest);
            }

            auto new_then = stmt_list();
            auto new_else = stmt_list();
            if (!tail(then_body, label, new_then, depth + 1) || !tail(else_body, label, new_else, depth + 1))
                return false;
            push_if(s, branch->condition, new_then, new_else, result);
            return true;
        }
        return true;
    }

    /// The label of a loop statement; `none` for any other.
    label_id loop_label_of(flat_stmt_id id) const
    {
        if (!is_known(out.e, id))
            return label_id::none;
        auto const& s = out.e.at(id);
        if (auto const* const l = s.node.try_as<flat_loop>())
            return l->label;
        if (auto const* const w = s.node.try_as<flat_while>())
            return w->label;
        if (auto const* const f = s.node.try_as<flat_for>())
            return f->label;
        return label_id::none;
    }

    /// `id` with every `leave $from` it holds, at any depth, as `leave $to`.
    flat_stmt_id retargeted(flat_stmt_id id, label_id from, label_id to, int depth = 0)
    {
        if (depth > k_max_depth || !holds_leave(out.e, id, from))
            return id;
        auto copy = out.e.at(id);
        auto const remapped = [&](ast::range_of<flat_stmt_id> range)
        {
            auto list = ids_of(out.e, range);
            for (auto& child : list)
                child = retargeted(child, from, to, depth + 1);
            return out.stmt_list(list);
        };
        if (auto* const leave = copy.node.try_as<flat_leave>())
            leave->target = to;
        else if (auto* const branch = copy.node.try_as<flat_if>())
        {
            branch->then_body = remapped(branch->then_body);
            branch->else_body = remapped(branch->else_body);
        }
        else if (auto* const block = copy.node.try_as<flat_block>())
            block->body = remapped(block->body);
        else if (auto* const loop = copy.node.try_as<flat_loop>())
            loop->body = remapped(loop->body);
        else if (auto* const w = copy.node.try_as<flat_while>())
            w->body = remapped(w->body);
        else if (auto* const f = copy.node.try_as<flat_for>())
            f->body = remapped(f->body);
        else if (auto* const once = copy.node.try_as<flat_once>())
            once->body = remapped(once->body);
        out.e.stmts.push_back(cc::move(copy));
        return flat_stmt_id(out.e.stmts.size() - 1);
    }

    stmt_list dissolve(stmt_list const& list, int depth = 0)
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

            if (auto const* const block = s.node.try_as<flat_block>())
            {
                auto body = dissolve(ids_of(out.e, block->body), depth + 1);
                // Rule X6: behind a loop that ends the block nothing runs, so leaving the block from inside it is leaving it.
                if (!body.empty() && is_valid(loop_label_of(body.back())))
                    body.back() = retargeted(body.back(), block->label, loop_label_of(body.back()));
                auto tailed = stmt_list();
                if (tail(body, block->label, tailed))
                    result.push_back_range(tailed);
                else
                {
                    out.from = s.from;
                    result.push_back(out.block(block->label, body));
                }
            }
            else if (auto const* const branch = s.node.try_as<flat_if>())
            {
                auto const then_body = dissolve(ids_of(out.e, branch->then_body), depth + 1);
                auto const else_body = dissolve(ids_of(out.e, branch->else_body), depth + 1);
                out.from = s.from;
                result.push_back(out.if_(branch->condition, then_body, else_body));
            }
            else if (auto const* const loop = s.node.try_as<flat_loop>())
            {
                auto const body = dissolve(ids_of(out.e, loop->body), depth + 1);
                out.from = s.from;
                result.push_back(out.loop(loop->label, body));
            }
            else if (auto const* const w = s.node.try_as<flat_while>())
            {
                auto const body = dissolve(ids_of(out.e, w->body), depth + 1);
                out.from = s.from;
                result.push_back(out.while_(w->label, w->condition, body));
            }
            else if (auto const* const f = s.node.try_as<flat_for>())
            {
                auto const body = dissolve(ids_of(out.e, f->body), depth + 1);
                out.from = s.from;
                result.push_back(out.for_(f->label, f->index, f->first, f->end, body));
            }
            else if (auto const* const sw = s.node.try_as<flat_switch>())
            {
                auto original = cc::vector<flat_arm>();
                original.push_back_range(out.e.at(sw->arms));
                auto arms = cc::vector<flat_arm>();
                for (auto const& a : original)
                    arms.push_back(
                        {.patterns = a.patterns, .body = out.stmt_list(dissolve(ids_of(out.e, a.body), depth + 1))});
                auto const default_body = out.stmt_list(dissolve(ids_of(out.e, sw->default_body), depth + 1));
                out.from = s.from;
                out.inlined_through = s.inlined_through;
                result.push_back(out.add_stmt(
                    flat_switch{.scrutinee = sw->scrutinee, .arms = out.arm_list(arms), .default_body = default_body}));
            }
            else
                result.push_back(id);
        }
        return result;
    }
};

/// Rules X1 and X3 to X5, top-down: every block left is a `once`, and every exit is spelled for where it stands.
struct exit_lowering
{
    flat_builder& out;
    legalize_options const& options;
    type_id bool_type = type_id::none;

    /// A flag an exit set on its way out, which the statement after a crossed construct has to test.
    struct pending_exit
    {
        local_id flag = local_id::none;
        label_id target = label_id::none;
        bool is_continue = false;
    };

    /// An enclosing `once` or loop.
    struct frame
    {
        label_id label = label_id::none;
        bool is_loop = false;
        local_id leave_flag = local_id::none;
        local_id continue_flag = local_id::none;
        cc::vector<pending_exit> pending;
    };
    cc::vector<frame> frames;

    isize frame_of(label_id label) const
    {
        for (auto i = frames.size() - 1; i >= 0; --i)
            if (frames[i].label == label)
                return i;
        return -1;
    }

    cc::string_view label_name(label_id label) const
    {
        return is_known(out.e, label) ? cc::string_view(out.e.at(label).name) : cc::string_view("block");
    }

    /// Rule X5: sets the flag of the target and breaks out of the innermost construct.
    void cross(isize target, bool is_continue, stmt_list& into)
    {
        auto& flag = is_continue ? frames[target].continue_flag : frames[target].leave_flag;
        if (!is_valid(flag))
            flag = add_temporary_var(
                out, cc::format("{}_{}", label_name(frames[target].label), is_continue ? "continued" : "left"),
                bool_type);
        auto const id = flag;

        for (auto i = target + 1; i < frames.size(); ++i)
        {
            auto is_listed = false;
            for (auto const& p : frames[i].pending)
                is_listed = is_listed || p.flag == id;
            if (!is_listed)
                frames[i].pending.push_back({.flag = id, .target = frames[target].label, .is_continue = is_continue});
        }

        into.push_back(out.assign(out.local(id), out.add_expr(bool_type, flat_bool_literal{.value = true})));
        into.push_back(out.break_());
    }

    flat_stmt_id declared_false(local_id flag)
    {
        return out.var(flag, out.add_expr(bool_type, flat_bool_literal{.value = false}));
    }

    /// A `once` or a loop: its flag in front of it, and behind it one test per exit that crossed it.
    template <class Make>
    void construct(flat_stmt const& s,
                   label_id label,
                   bool is_loop,
                   ast::range_of<flat_stmt_id> body,
                   stmt_list& into,
                   int depth,
                   Make&& make)
    {
        frames.push_back({.label = label, .is_loop = is_loop});
        auto inner = lower(ids_of(out.e, body), depth + 1);
        auto const done = frames.pop_back();

        out.from = s.from;
        out.inlined_through = s.inlined_through;
        // A `once` ends where its body ends, so a `break` as its last statement says nothing.
        if (!is_loop && !inner.empty() && is_known(out.e, inner.back()) && out.e.at(inner.back()).node.is<flat_break>())
            inner.remove_back();
        if (is_valid(done.continue_flag))
            inner.insert_at(0, declared_false(done.continue_flag));
        if (is_valid(done.leave_flag))
            into.push_back(declared_false(done.leave_flag));
        into.push_back(make(inner));

        for (auto const& p : done.pending)
        {
            if (options.skip_flag_tests)
                break;
            auto const is_arrived = !frames.empty() && frames.back().label == p.target;
            auto const exit = is_arrived && p.is_continue ? out.continue_(p.target) : out.break_();
            into.push_back(out.if_(out.local(p.flag), {exit}));
        }
    }

    /// A `switch`: one frame for the whole construct, since a `break` in any arm ends it (LEGAL-3).
    void lower_switch(flat_stmt const& s, flat_switch const& sw, stmt_list& into, int depth)
    {
        frames.push_back({.label = label_id::none, .is_loop = false});
        auto original = cc::vector<flat_arm>();
        original.push_back_range(out.e.at(sw.arms));
        auto arms = cc::vector<flat_arm>();
        for (auto const& a : original)
            arms.push_back({.patterns = a.patterns, .body = out.stmt_list(lower(ids_of(out.e, a.body), depth + 1))});
        auto const default_body = out.stmt_list(lower(ids_of(out.e, sw.default_body), depth + 1));
        auto const done = frames.pop_back();

        out.from = s.from;
        out.inlined_through = s.inlined_through;
        if (is_valid(done.leave_flag))
            into.push_back(declared_false(done.leave_flag));
        into.push_back(out.add_stmt(
            flat_switch{.scrutinee = sw.scrutinee, .arms = out.arm_list(arms), .default_body = default_body}));

        for (auto const& p : done.pending)
        {
            if (options.skip_flag_tests)
                break;
            auto const is_arrived = !frames.empty() && frames.back().label == p.target;
            auto const exit = is_arrived && p.is_continue ? out.continue_(p.target) : out.break_();
            into.push_back(out.if_(out.local(p.flag), {exit}));
        }
    }

    stmt_list lower(stmt_list const& list, int depth = 0)
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

            if (auto const* const leave = s.node.try_as<flat_leave>())
            {
                auto const target = frame_of(leave->target);
                if (is_valid(out.e.root) && leave->target == out.e.root)
                    result.push_back(out.return_(leave->value));
                else if (target < 0)
                    result.push_back(id);
                else if (target == frames.size() - 1)
                    result.push_back(out.break_());
                else
                    cross(target, false, result);
                return result;
            }
            if (auto const* const c = s.node.try_as<flat_continue>())
            {
                auto const target = frame_of(c->target);
                if (target < 0 || !frames[target].is_loop || target == frames.size() - 1)
                    result.push_back(id);
                else
                    cross(target, true, result);
                return result;
            }
            if (is_exit(s))
            {
                result.push_back(id);
                return result;
            }

            if (auto const* const block = s.node.try_as<flat_block>())
                construct(s, block->label, false, block->body, result, depth,
                          [&](stmt_list const& body) { return out.once(body); });
            else if (auto const* const loop = s.node.try_as<flat_loop>())
                construct(s, loop->label, true, loop->body, result, depth,
                          [&](stmt_list const& body) { return out.loop(loop->label, body); });
            else if (auto const* const w = s.node.try_as<flat_while>())
                construct(s, w->label, true, w->body, result, depth,
                          [&](stmt_list const& body) { return out.while_(w->label, w->condition, body); });
            else if (auto const* const f = s.node.try_as<flat_for>())
                construct(s, f->label, true, f->body, result, depth,
                          [&](stmt_list const& body) { return out.for_(f->label, f->index, f->first, f->end, body); });
            else if (auto const* const branch = s.node.try_as<flat_if>())
            {
                auto const then_body = lower(ids_of(out.e, branch->then_body), depth + 1);
                auto const else_body = lower(ids_of(out.e, branch->else_body), depth + 1);
                out.from = s.from;
                result.push_back(out.if_(branch->condition, then_body, else_body));
            }
            else if (auto const* const sw = s.node.try_as<flat_switch>())
                lower_switch(s, *sw, result, depth);
            else
                result.push_back(id);
        }
        return result;
    }
};

/// Copies what the body reaches into a fresh tree.
struct compactor
{
    flat_entry_point const& in;
    flat_builder out;

    flat_expr_id expr(flat_expr_id id, int depth)
    {
        if (!is_known(in, id) || depth > k_max_depth)
            return flat_expr_id::none;
        auto copy = in.at(id);
        if (auto* const member = copy.node.try_as<flat_member>())
            member->object = expr(member->object, depth + 1);
        else if (auto* const element = copy.node.try_as<flat_buffer_element>())
        {
            element->buffer = expr(element->buffer, depth + 1);
            element->index = expr(element->index, depth + 1);
        }
        else if (auto* const construct = copy.node.try_as<flat_construct>())
            construct->arguments = exprs(construct->arguments, depth);
        else if (auto* const call = copy.node.try_as<flat_call>())
            call->arguments = exprs(call->arguments, depth);
        else if (auto* const n = copy.node.try_as<flat_not>())
            n->operand = expr(n->operand, depth + 1);
        else if (auto* const a = copy.node.try_as<flat_and>())
        {
            a->lhs = expr(a->lhs, depth + 1);
            a->rhs = expr(a->rhs, depth + 1);
        }
        else if (auto* const o = copy.node.try_as<flat_or>())
        {
            o->lhs = expr(o->lhs, depth + 1);
            o->rhs = expr(o->rhs, depth + 1);
        }
        else if (auto* const block = copy.node.try_as<flat_block>())
            block->body = body(ids_of(in, block->body), depth + 1);
        out.e.exprs.push_back(cc::move(copy));
        return flat_expr_id(out.e.exprs.size() - 1);
    }

    ast::range_of<flat_expr_id> exprs(ast::range_of<flat_expr_id> range, int depth)
    {
        auto list = cc::vector<flat_expr_id>();
        if (is_known(in, range))
            for (auto const id : in.at(range))
                list.push_back(expr(id, depth + 1));
        return out.expr_list(list);
    }

    flat_expr_id optional_expr(flat_expr_id id, int depth) { return is_valid(id) ? expr(id, depth) : id; }

    ast::range_of<flat_arm> arms(ast::range_of<flat_arm> range, int depth)
    {
        auto list = cc::vector<flat_arm>();
        if (is_known(in, range))
            for (auto const& arm : in.at(range))
                list.push_back({.patterns = exprs(arm.patterns, depth), .body = body(ids_of(in, arm.body), depth + 1)});
        return out.arm_list(list);
    }

    ast::range_of<flat_stmt_id> body(stmt_list const& list, int depth)
    {
        auto copies = stmt_list();
        for (auto const id : list)
        {
            if (!is_known(in, id) || depth > k_max_depth)
                continue;
            auto copy = in.at(id);
            copy.node.visit([&](flat_let& n) { n.value = expr(n.value, 0); }, //
                            [&](flat_var& n) { n.value = optional_expr(n.value, 0); },
                            [&](flat_assign& n)
                            {
                                n.place = expr(n.place, 0);
                                n.value = expr(n.value, 0);
                            },
                            [&](flat_print& n) { n.value = expr(n.value, 0); }, //
                            [&](flat_eval& n) { n.value = expr(n.value, 0); },
                            [&](flat_if& n)
                            {
                                n.condition = expr(n.condition, 0);
                                n.then_body = body(ids_of(in, n.then_body), depth + 1);
                                n.else_body = body(ids_of(in, n.else_body), depth + 1);
                            },
                            [&](flat_block& n) { n.body = body(ids_of(in, n.body), depth + 1); },
                            [&](flat_leave& n) { n.value = optional_expr(n.value, 0); },
                            [&](flat_loop& n) { n.body = body(ids_of(in, n.body), depth + 1); },
                            [&](flat_while& n)
                            {
                                n.condition = expr(n.condition, 0);
                                n.body = body(ids_of(in, n.body), depth + 1);
                            },
                            [&](flat_for& n)
                            {
                                n.first = expr(n.first, 0);
                                n.end = expr(n.end, 0);
                                n.body = body(ids_of(in, n.body), depth + 1);
                            },
                            [&](flat_continue&) {},                                                                   //
                            [&](flat_once& n) { n.body = body(ids_of(in, n.body), depth + 1); }, [&](flat_break&) {}, //
                            [&](flat_case& n)
                            {
                                n.scrutinee = expr(n.scrutinee, 0);
                                n.arms = arms(n.arms, depth);
                                n.default_body = body(ids_of(in, n.default_body), depth + 1);
                            },
                            [&](flat_switch& n)
                            {
                                n.scrutinee = expr(n.scrutinee, 0);
                                n.arms = arms(n.arms, depth);
                                n.default_body = body(ids_of(in, n.default_body), depth + 1);
                            },
                            [&](flat_return& n) { n.value = expr(n.value, 0); });
            out.e.stmts.push_back(cc::move(copy));
            copies.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return out.stmt_list(copies);
    }
};
} // namespace

stmt_list sgl::check::impl::lower_exits(flat_builder& out, stmt_list const& body, legalize_options const& options)
{
    auto const dissolved = block_dissolver{.out = out}.dissolve(body);
    auto lowering = exit_lowering{.out = out, .options = options, .bool_type = out.type_named(builtins::k_bool)};
    return lowering.lower(dissolved);
}

flat_entry_point sgl::check::impl::compacted(checked_module const& m, flat_entry_point const& e, stmt_list const& body)
{
    // Everything but the tree itself is carried as it is, so a field added to the entry point later survives by default.
    auto header = e;
    header.exprs.clear();
    header.stmts.clear();
    header.expr_lists.clear();
    header.stmt_lists.clear();
    header.arms.clear();
    header.body = {};
    auto c = compactor{.in = e, .out = flat_builder::extend(m, cc::move(header))};
    c.out.e.body = c.body(body, 0);
    return cc::move(c.out.e);
}

flat_entry_point sgl::check::legalize(checked_module const& m, flat_entry_point const& e, legalize_options const& options)
{
    if (is_core(e))
        return e;
    auto out = flat_builder::extend(m, e);
    out.set_body(lower_expressions(out, options));
    out.set_body(lower_cases(out));
    // Again, for the conditions the chain form of C1 wrote; on a tree that holds no block it changes nothing.
    auto const without_blocks = lower_expressions(out, options);
    auto const without_leaves = lower_exits(out, without_blocks, options);
    auto const without_void = erase_void(out, without_leaves);
    return compacted(m, out.e, without_void);
}
