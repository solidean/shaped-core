#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// CHK-538: an index into a local array is stated, or the array's let says how it is held.
// The pass reads the structured tree with every call inlined, so a parameter is whatever its call passed.

namespace
{
/// The local the place `id` is part of, through its members and elements; none for any other place.
local_id root_of(flat_entry_point const& e, flat_expr_id id)
{
    for (auto depth = 0; is_known(e, id) && depth < k_max_depth; ++depth)
    {
        auto const& node = e.at(id).node;
        if (auto const* const ref = node.try_as<flat_local_ref>())
            return ref->local;
        if (auto const* const member = node.try_as<flat_member>())
            id = member->object;
        else if (auto const* const element = node.try_as<flat_element>())
            id = element->object;
        else
            return local_id::none;
    }
    return local_id::none;
}

/// What decides whether an expression is stated: a value built of constants, loop counters and the locals so built.
struct statedness
{
    checked_module const& m;
    flat_entry_point const& e;
    /// Parallel to `e.locals`: whether every value the local holds is stated; only ever turns false.
    cc::vector<bool> is_stated;
    /// Parallel to `e.locals`: the values a local is given, by its declaration, an assignment or an index of one.
    cc::vector<cc::vector<flat_expr_id>> given;
    /// Parallel to `e.labels`: the values a block expression is left with.
    cc::vector<cc::vector<flat_expr_id>> left_with;
    /// Parallel to `e.exprs`, for one round: 0 unknown, 1 stated, 2 not.
    cc::vector<u8> memo;

    [[nodiscard]] local_id root_of(flat_expr_id id) const { return ::root_of(e, id); }

    void collect()
    {
        is_stated = cc::vector<bool>::create_filled(e.locals.size(), true);
        given = cc::vector<cc::vector<flat_expr_id>>::create_defaulted(e.locals.size());
        left_with = cc::vector<cc::vector<flat_expr_id>>::create_defaulted(e.labels.size());
        // what the stage is handed is data
        for (auto i = isize(0); i < e.locals.size(); ++i)
            if (e.locals[i].kind == local_kind::parameter)
                is_stated[i] = false;
        for (auto const& input : e.stage_inputs)
            if (is_known(e, input.local))
                is_stated[index_of(input.local)] = false;

        walked = cc::vector<bool>::create_filled(e.exprs.size(), false);
        walk_body(e.body, 0);
        // a builtin that writes through a `mut` parameter writes what it computes, which nothing here follows
        for (auto const& x : e.exprs)
        {
            auto const* const call = x.node.try_as<flat_call>();
            if (call == nullptr || !is_known(e, call->arguments) || !is_valid(call->callee))
                continue;
            auto const& symbol = m.at(call->callee);
            if (symbol.info < 0 || symbol.info >= m.functions.size())
                continue;
            auto const parameters = m.at(m.functions[symbol.info].parameters);
            auto const arguments = e.at(call->arguments);
            for (auto k = isize(0); k < parameters.size() && k < arguments.size(); ++k)
                if (parameters[k].is_mut)
                    if (auto const root = root_of(arguments[k]); is_known(e, root))
                        is_stated[index_of(root)] = false;
        }
    }

    /// The conditions of the branches and loops around the statement being walked, outermost first.
    cc::vector<flat_expr_id> conditions;
    /// Where the innermost block expression's own conditions start among `conditions`.
    isize block_base = 0;
    cc::vector<bool> walked;

    /// `value` given to `local` where the walk stands: a value stored under a branch depends on what the branch tests.
    void give(local_id local, flat_expr_id value, isize from_condition = 0)
    {
        if (!is_known(e, local) || !is_valid(value))
            return;
        given[index_of(local)].push_back(value);
        for (auto i = from_condition; i < conditions.size(); ++i)
            given[index_of(local)].push_back(conditions[i]);
    }

    void walk_expr(flat_expr_id id, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth || walked[index_of(id)])
            return;
        walked[index_of(id)] = true;
        auto const& x = e.at(id);
        if (auto const* const b = x.node.try_as<flat_block>())
        {
            // a block's value depends on the branches inside it, not on those around where it stands
            auto const base = block_base;
            block_base = conditions.size();
            walk_body(b->body, depth + 1);
            block_base = base;
        }
        if (auto const* const both = x.node.try_as<flat_by_target>())
        {
            walk_expr(both->native, depth + 1);
            walk_expr(both->emulated, depth + 1);
        }
        for_each_operand(e, x, [&](flat_expr_id operand) { walk_expr(operand, depth + 1); });
    }

    void walk_under(flat_expr_id condition, ast::range_of<flat_stmt_id> range, int depth)
    {
        conditions.push_back(condition);
        walk_body(range, depth);
        conditions.remove_back();
    }

    void walk_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e, range) || depth > k_max_depth)
            return;
        for (auto const id : e.at(range))
        {
            if (!is_known(e, id))
                continue;
            auto const& s = e.at(id);
            for_each_expr_of(e, s, [&](flat_expr_id x) { walk_expr(x, depth + 1); });
            for_each_pattern_of(e, s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e, patterns))
                                        for (auto const p : e.at(patterns))
                                            walk_expr(p, depth + 1);
                                });
            // a let is its value where it stands, whatever branch it stands in (CHK-399)
            if (auto const* const let = s.node.try_as<flat_let>())
                give(let->local, let->value, conditions.size());
            else if (auto const* const var = s.node.try_as<flat_var>())
                give(var->local, var->value);
            else if (auto const* const leave = s.node.try_as<flat_leave>())
            {
                if (is_known(e, leave->target) && is_valid(leave->value))
                {
                    auto& values = left_with[index_of(leave->target)];
                    values.push_back(leave->value);
                    for (auto i = block_base; i < conditions.size(); ++i)
                        values.push_back(conditions[i]);
                }
            }
            else if (auto const* const assign = s.node.try_as<flat_assign>())
            {
                // a part stored at an index computed from data makes the whole local depend on it
                auto const root = root_of(assign->place);
                give(root, assign->value);
                for (auto place = assign->place; is_known(e, place) && !e.at(place).node.is<flat_local_ref>();)
                {
                    auto const& node = e.at(place).node;
                    if (auto const* const element = node.try_as<flat_element>())
                    {
                        give(root, element->index);
                        place = element->object;
                    }
                    else if (auto const* const member = node.try_as<flat_member>())
                        place = member->object;
                    else
                        break;
                }
            }
            if (auto const* const branch = s.node.try_as<flat_if>())
            {
                walk_under(branch->condition, branch->then_body, depth + 1);
                walk_under(branch->condition, branch->else_body, depth + 1);
            }
            else if (auto const* const loop = s.node.try_as<flat_while>())
                walk_under(loop->condition, loop->body, depth + 1);
            else if (auto const* const c = s.node.try_as<flat_case>())
                for_each_body_of(
                    e, s, [&](ast::range_of<flat_stmt_id> inner) { walk_under(c->scrutinee, inner, depth + 1); });
            else if (auto const* const sw = s.node.try_as<flat_switch>())
                for_each_body_of(
                    e, s, [&](ast::range_of<flat_stmt_id> inner) { walk_under(sw->scrutinee, inner, depth + 1); });
            else
                for_each_body_of(e, s, [&](ast::range_of<flat_stmt_id> inner) { walk_body(inner, depth + 1); });
        }
    }

    [[nodiscard]] bool all_stated(cc::span<flat_expr_id const> values, int depth)
    {
        for (auto const v : values)
            if (!stated(v, depth + 1))
                return false;
        return true;
    }

    [[nodiscard]] bool stated(flat_expr_id id, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth)
            return false;
        auto& known = memo[index_of(id)];
        if (known != 0)
            return known == 1;
        // a cycle through a block's leaves reads as stated until it settles, as a local does
        known = 1;
        auto const result = judge(id, depth);
        memo[index_of(id)] = result ? 1 : 2;
        return result;
    }

    [[nodiscard]] bool judge(flat_expr_id id, int depth)
    {
        auto const& x = e.at(id);
        auto const operands_stated = [&]
        {
            auto result = true;
            for_each_operand(e, x, [&](flat_expr_id operand) { result = result && stated(operand, depth + 1); });
            return result;
        };
        if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_wide_literal>()
            || x.node.is<flat_bool_literal>() || x.node.is<flat_enum_value>())
            return true;
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
            return is_known(e, ref->local) && is_stated[index_of(ref->local)];
        if (x.node.is<flat_member>() || x.node.is<flat_element>() || x.node.is<flat_construct>()
            || x.node.is<flat_not>() || x.node.is<flat_and>() || x.node.is<flat_or>())
            return operands_stated();
        if (auto const* const call = x.node.try_as<flat_call>())
        {
            // a builtin of no argument reads what the invocation is: its lane, its launch, its ray
            if (!call->is_pure || call->text >= 0 || call->arguments.count == 0)
                return false;
            return operands_stated();
        }
        if (auto const* const block = x.node.try_as<flat_block>())
            return is_known(e, block->label) && all_stated(left_with[index_of(block->label)], depth);
        if (auto const* const both = x.node.try_as<flat_by_target>())
            return stated(both->native, depth + 1) && stated(both->emulated, depth + 1);
        // a binding member, a buffer element, a length, the shadow of workgroup memory: what the host or the GPU wrote
        return false;
    }

    void settle()
    {
        // a local only ever turns unstated, so the rounds settle within one per local
        for (auto round = isize(0); round <= e.locals.size(); ++round)
        {
            memo = cc::vector<u8>::create_filled(e.exprs.size(), 0);
            auto is_changed = false;
            for (auto i = isize(0); i < e.locals.size(); ++i)
            {
                if (!is_stated[i] || e.locals[i].kind == local_kind::index)
                    continue;
                if (!all_stated(given[i], 0))
                {
                    is_stated[i] = false;
                    is_changed = true;
                }
            }
            if (!is_changed)
                break;
        }
        memo = cc::vector<u8>::create_filled(e.exprs.size(), 0);
    }
};

/// EMIT-230: a literal, or an array or a vector built of nothing else, which a target holds as constant data.
bool is_constant_leaf(checked_module const& m, flat_entry_point const& e, flat_expr_id id, int depth)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return false;
    auto const& x = e.at(id);
    if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_wide_literal>()
        || x.node.is<flat_bool_literal>())
        return true;
    auto const* const c = x.node.try_as<flat_construct>();
    if (c == nullptr || !is_known(e, c->arguments))
        return false;
    auto const& t = m.at(x.type);
    if (t.kind != type_kind::array && m.builtin_type_of(x.type) == nullptr)
        return false;
    for (auto const a : e.at(c->arguments))
        if (!is_constant_leaf(m, e, a, depth + 1))
            return false;
    return true;
}

/// The reachable expressions of `e`, each once, in the order a walk meets them.
struct reach_walk
{
    flat_entry_point const& e;
    cc::vector<bool> is_seen;
    cc::vector<flat_expr_id> found;

    void expr(flat_expr_id id, int depth)
    {
        if (!is_known(e, id) || depth > k_max_depth || is_seen[index_of(id)])
            return;
        is_seen[index_of(id)] = true;
        found.push_back(id);
        auto const& x = e.at(id);
        if (auto const* const b = x.node.try_as<flat_block>())
            body(b->body, depth + 1);
        if (auto const* const both = x.node.try_as<flat_by_target>())
        {
            expr(both->native, depth + 1);
            expr(both->emulated, depth + 1);
        }
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
} // namespace

void checker::judge_local_indices(flat_entry_point& structured)
{
    auto& e = structured;
    // a copy of an annotated array, such as a parameter bound to it, is held as that array is
    auto const inherit = [&](local_id local, flat_expr_id value)
    {
        if (!is_known(e, local) || !is_known(e, value) || e.at(local).indexing != local_indexing::unstated)
            return false;
        auto const* const ref = e.at(value).node.try_as<flat_local_ref>();
        if (ref == nullptr || !is_known(e, ref->local) || e.at(ref->local).indexing == local_indexing::unstated
            || out.at(e.at(local).type).kind != type_kind::array)
            return false;
        e.locals[index_of(local)].indexing = e.at(ref->local).indexing;
        return true;
    };
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto const& s : e.stmts)
        {
            if (auto const* const let = s.node.try_as<flat_let>())
                changed = inherit(let->local, let->value) || changed;
            else if (auto const* const var = s.node.try_as<flat_var>())
                changed = inherit(var->local, var->value) || changed;
        }
    }

    // LEGAL-72 writes an element of a `.select` array as a choice among locals, which is a value and no place a builtin
    // could write through, so such an argument is refused rather than lowered as a read.
    // No builtin reaches it yet: a builtin's parameter is never `is_mut`, its resources and atomics being no places.
    for (auto const& x : e.exprs)
    {
        auto const* const call = x.node.try_as<flat_call>();
        if (call == nullptr || !is_known(e, call->arguments) || !is_valid(call->callee))
            continue;
        auto const& symbol = out.at(call->callee);
        if (symbol.info < 0 || symbol.info >= out.functions.size())
            continue;
        auto const parameters = out.at(out.functions[symbol.info].parameters);
        auto const arguments = e.at(call->arguments);
        for (auto k = isize(0); k < parameters.size() && k < arguments.size(); ++k)
        {
            if (!parameters[k].is_mut)
                continue;
            auto const root = root_of(e, arguments[k]);
            if (!is_known(e, root) || e.at(root).indexing != local_indexing::select || !ast::is_valid(x.from.expr))
                continue;
            auto const& local = e.at(root);
            auto const name
                = local.source_name.empty() ? cc::string_view(local.name) : cc::string_view(local.source_name);
            auto& d = report(diagnostic_kind::unsupported_yet, x.from.file, span_of(x.from.file, x.from.expr),
                             cc::format("a place of `{}`, an `@indexed(.select)` array, handed to {}'s `mut` "
                                        "parameter, which writes through it; bind the element to a `let mut` local, "
                                        "pass that, and store it back",
                                        name, symbol.name));
            d.entry_points.push_back(e.function);
        }
    }

    // most trees index no local array at run time, and judging one is not free
    auto const is_local_array = [&](local_id id)
    {
        if (!is_known(e, id))
            return false;
        auto const& local = e.at(id);
        return local.kind != local_kind::parameter && local.kind != local_kind::index
            && out.at(local.type).kind == type_kind::array;
    };
    auto any = false;
    for (auto const& x : e.exprs)
        if (auto const* const element = x.node.try_as<flat_element>();
            element != nullptr && !e.at(element->index).node.is<flat_int_literal>())
            any = true;
    if (!any)
        return;

    auto pass = statedness{.m = out, .e = e};
    pass.collect();
    pass.settle();

    auto walk = reach_walk{.e = e, .is_seen = cc::vector<bool>::create_filled(e.exprs.size(), false)};
    walk.body(e.body, 0);

    auto declared_at = cc::vector<flat_stmt_id>::create_filled(e.locals.size(), flat_stmt_id::none);
    for (auto i = isize(0); i < e.stmts.size(); ++i)
    {
        auto const local = e.stmts[i].node.is<flat_let>() ? e.stmts[i].node.as<flat_let>().local
                         : e.stmts[i].node.is<flat_var>() ? e.stmts[i].node.as<flat_var>().local
                                                          : local_id::none;
        if (is_known(e, local) && !is_valid(declared_at[index_of(local)]))
            declared_at[index_of(local)] = flat_stmt_id(i32(i));
    }

    auto reported = cc::vector<origin>();
    for (auto const id : walk.found)
    {
        auto const& x = e.at(id);
        auto const* const element = x.node.try_as<flat_element>();
        if (element == nullptr || !is_known(e, element->object)
            || out.at(e.at(element->object).type).kind != type_kind::array)
            continue;
        auto const root = pass.root_of(element->object);
        if (!is_local_array(root) || e.at(root).indexing != local_indexing::unstated)
            continue;
        // EMIT-230: a table of constants nothing writes is constant data, and costs no memory per invocation
        auto const declared = declared_at[index_of(root)];
        if (is_valid(declared) && e.at(declared).node.is<flat_let>()
            && is_constant_leaf(out, e, e.at(declared).node.as<flat_let>().value, 0))
            continue;
        if (pass.stated(element->index, 0))
            continue;

        // the index as the program wrote it: the subscript's own argument where it has one
        auto at = e.at(element->index).from;
        if (ast::is_valid(x.from.expr))
            if (auto const* const subscript = ast_of(x.from.file).at(x.from.expr).node.try_as<ast::index>();
                subscript != nullptr && ast_of(x.from.file).at(subscript->arguments).size() == 1)
                at = {.file = x.from.file, .expr = ast_of(x.from.file).at(subscript->arguments)[0].value};
        if (!ast::is_valid(at.expr))
            at = x.from;
        if (!ast::is_valid(at.expr))
            continue;
        auto is_seen = false;
        for (auto const& r : reported)
            is_seen = is_seen || r == at;
        if (is_seen)
            continue;
        reported.push_back(at);

        auto const& local = e.at(root);
        auto const name = local.source_name.empty() ? cc::string_view(local.name) : cc::string_view(local.source_name);
        auto const where = span_of(at.file, at.expr);
        auto& d = report(diagnostic_kind::indexing_not_stated, at.file, where,
                         cc::format("`{}` indexes the local array `{}` by a value computed from data rather than from "
                                    "constants and loop counters, so a target may keep the array in memory; state on "
                                    "its let how it is held: `@indexed(.select)` keeps one value per element and "
                                    "chooses among them, `@indexed(.memory)` keeps the array",
                                    text_of(at.file, where), name));
        // CHK-402: a compile of another entry point does not report it
        d.entry_points.push_back(e.function);
        if (is_valid(declared))
        {
            auto const& from = e.at(declared).from;
            if (ast::is_valid(from.stmt) || ast::is_valid(from.expr))
                d.notes.push_back(
                    {.file = from.file,
                     .where = ast::is_valid(from.stmt) ? span_of(from.file, from.stmt) : span_of(from.file, from.expr),
                     .message = cc::format("`{}` is declared here", name)});
        }
    }
}
