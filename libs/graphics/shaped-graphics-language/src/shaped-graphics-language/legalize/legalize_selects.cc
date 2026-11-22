#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// LEGAL-72: a local array its let states `@indexed(.select)` is one local per element.
// A read at a run-time index is a choice among them, and a store one assignment per element, each under its test.
// It runs after the guards of LEGAL-65, so an index that reaches it is in range wherever the profile guards it,
// and with no profile the last element takes every other index, which EVAL-90 leaves without a value.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
stmt_list ids_of(flat_entry_point const& e, ast::range_of<flat_stmt_id> range)
{
    auto result = stmt_list();
    if (is_known(e, range))
        for (auto const id : e.at(range))
            result.push_back(id);
    return result;
}

/// Whether `local` of `e` is lowered: a `.select` array of one dimension and at most CHK-538's elements.
bool is_lowered(checked_module const& m, flat_entry_point const& e, flat_local const& local)
{
    if (local.indexing != local_indexing::select || local.kind == local_kind::parameter || local.kind == local_kind::index)
        return false;
    auto const& t = m.at(local.type);
    return t.kind == type_kind::array && t.count >= 1 && t.count <= k_max_selected_elements
        && m.at(t.element).kind != type_kind::array;
}

struct selector
{
    flat_builder& out;
    /// Parallel to the locals the tree started with: the local of each element, empty for one that is not lowered.
    cc::vector<cc::vector<local_id>> parts;

    cc::vector<local_id> none;

    [[nodiscard]] cc::vector<local_id> const& parts_of(local_id id) const
    {
        if (!is_valid(id) || index_of(id) >= parts.size())
            return none;
        return parts[index_of(id)];
    }

    /// The lowered local `id` names directly, as `local_ref`; none for anything else.
    [[nodiscard]] local_id lowered_ref(flat_expr_id id) const
    {
        if (!is_known(out.e, id))
            return local_id::none;
        auto const* const ref = out.e.at(id).node.try_as<flat_local_ref>();
        return ref != nullptr && !parts_of(ref->local).empty() ? ref->local : local_id::none;
    }

    void at(flat_expr const& x)
    {
        out.from = x.from;
        out.inlined_through = x.inlined_through;
    }
    void at(flat_stmt const& s)
    {
        out.from = s.from;
        out.inlined_through = s.inlined_through;
    }

    [[nodiscard]] flat_expr_id literal_like(flat_expr_id index, i32 k)
    {
        auto const type = out.e.at(index).type;
        return out.add_expr(type, flat_int_literal{.value = k, .is_unsigned = out.m.name_of(type) == builtins::k_uint});
    }

    /// `index == k`, by the `==` of the index's type.
    [[nodiscard]] flat_expr_id is_at(local_id index, i32 k)
    {
        auto const ref = out.local(index);
        auto const family = out.m.name_of(out.e.at(index).type) == builtins::k_uint ? "uint" : "int";
        flat_expr_id const arguments[] = {ref, literal_like(ref, k)};
        return out.call(cc::format("equal_{}", family), arguments);
    }

    /// The literal `id` is, held to the array's elements; -1 for an index that is no literal.
    [[nodiscard]] i32 literal_index(flat_expr_id id, isize count) const
    {
        auto const* const literal = out.e.at(id).node.try_as<flat_int_literal>();
        if (literal == nullptr)
            return -1;
        return literal->value < 0 ? 0 : literal->value >= count ? i32(count - 1) : literal->value;
    }

    /// Element `k` of `value`, an array of `count` elements: an argument of its construction, a part of a lowered
    /// local, or else an element of `held`, the local `value` was bound to.
    [[nodiscard]] flat_expr_id element_of(flat_expr_id value, local_id held, i32 k)
    {
        if (!is_valid(held))
        {
            if (auto const* const c = out.e.at(value).node.try_as<flat_construct>())
                return out.e.at(c->arguments)[k];
            if (auto const lowered = lowered_ref(value); is_valid(lowered))
                return out.local(parts_of(lowered)[k]);
        }
        auto const element_type = out.m.at(out.e.at(held).type).element;
        return out.add_expr(element_type, flat_element{.object = out.local(held), .index = out.int_literal(k)});
    }

    /// Whether `value` hands each element on as it is, so no local must hold it first.
    [[nodiscard]] bool hands_elements(flat_expr_id value, isize count) const
    {
        if (is_valid(lowered_ref(value)))
            return true;
        auto const* const c = out.e.at(value).node.try_as<flat_construct>();
        if (c == nullptr || c->arguments.count != count)
            return false;
        // an argument named twice, as a `filled` value is, is evaluated once
        auto const arguments = out.e.at(c->arguments);
        for (auto i = isize(0); i < arguments.size(); ++i)
            for (auto j = i + 1; j < arguments.size(); ++j)
                if (arguments[i] == arguments[j])
                    return false;
        return true;
    }

    /// The elements of `value` for the parts of a lowered local, the statements that bind it first into `into`.
    cc::vector<flat_expr_id> elements_of(flat_expr_id value, isize count, stmt_list& into)
    {
        auto held = local_id::none;
        if (!hands_elements(value, count))
        {
            held = out.add_local(local_kind::temporary, "elements", out.e.at(value).type);
            into.push_back(out.let(held, value));
        }
        auto result = cc::vector<flat_expr_id>();
        for (auto k = 0; k < count; ++k)
            result.push_back(element_of(value, held, k));
        return result;
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    void declare(flat_stmt const& s, local_id local, flat_expr_id value, stmt_list& into)
    {
        auto const elements = parts_of(local);
        auto const own = cc::vector<local_id>(elements);
        auto values = cc::vector<flat_expr_id>();
        if (is_valid(value))
            values = elements_of(value, own.size(), into);
        at(s);
        for (auto k = isize(0); k < own.size(); ++k)
        {
            auto const v = values.empty() ? flat_expr_id::none : values[k];
            if (out.e.at(own[k]).is_mut)
                into.push_back(out.var(own[k], v));
            else
                into.push_back(out.let(own[k], v));
        }
    }

    /// One step of a place above the element of the lowered local: a member, a swizzle or an element.
    struct step
    {
        flat_expr node;
        /// For an element whose index is no literal, the local it was pinned into.
        local_id pinned = local_id::none;
    };

    /// `place` written as an assignment of `value` into a lowered local, or false for a place rooted elsewhere.
    bool assign(flat_stmt const& s, flat_expr_id place, flat_expr_id value, stmt_list& into)
    {
        // the steps from the place down to the element of the lowered local, outermost first
        auto steps = cc::vector<step>();
        auto id = place;
        auto root = local_id::none;
        auto index = flat_expr_id::none;
        for (auto depth = 0; depth < k_max_depth && is_known(out.e, id); ++depth)
        {
            auto const& x = out.e.at(id);
            if (auto const lowered = lowered_ref(id); is_valid(lowered))
            {
                root = lowered;
                break;
            }
            if (auto const* const element = x.node.try_as<flat_element>())
            {
                if (auto const lowered = lowered_ref(element->object); is_valid(lowered))
                {
                    root = lowered;
                    index = element->index;
                    break;
                }
                steps.push_back({.node = x});
                id = element->object;
            }
            else if (auto const* const member = x.node.try_as<flat_member>())
            {
                steps.push_back({.node = x});
                id = member->object;
            }
            else
                return false;
        }
        if (!is_valid(root))
            return false;
        auto const elements = cc::vector<local_id>(parts_of(root));
        auto const count = elements.size();

        // the whole local: each part takes its element
        if (!is_valid(index))
        {
            auto const values = elements_of(value, count, into);
            at(s);
            for (auto k = isize(0); k < count; ++k)
                into.push_back(out.assign(out.local(elements[k]), values[k]));
            return true;
        }

        // `p` with the steps above the element applied, innermost last in the list and first applied
        auto const rebuilt = [&](local_id p)
        {
            auto current = out.local(p);
            for (auto i = steps.size() - 1; i >= 0; --i)
            {
                auto node = steps[i].node;
                if (auto* const member = node.node.try_as<flat_member>())
                    member->object = current;
                else if (auto* const element = node.node.try_as<flat_element>())
                {
                    element->object = current;
                    if (is_valid(steps[i].pinned))
                        element->index = out.local(steps[i].pinned);
                }
                out.e.exprs.push_back(cc::move(node));
                current = flat_expr_id(out.e.exprs.size() - 1);
            }
            return current;
        };

        if (auto const k = literal_index(index, count); k >= 0)
        {
            at(s);
            into.push_back(out.assign(rebuilt(elements[k]), value));
            return true;
        }

        // the index first, then each index above it from the innermost out, then the value (EVAL-14)
        at(s);
        auto const chosen = out.add_local(local_kind::temporary, "chosen", out.e.at(index).type);
        into.push_back(out.let(chosen, index));
        for (auto i = steps.size() - 1; i >= 0; --i)
            if (auto const* const element = steps[i].node.node.try_as<flat_element>();
                element != nullptr && !out.e.at(element->index).node.is<flat_int_literal>())
            {
                steps[i].pinned = out.add_local(local_kind::temporary, "index", out.e.at(element->index).type);
                into.push_back(out.let(steps[i].pinned, element->index));
            }
        auto const stored = out.add_local(local_kind::temporary, "stored", out.e.at(value).type);
        into.push_back(out.let(stored, value));

        // `if chosen == 0 { … } else { if chosen == 1 { … } else { … } }`, the last element taking the rest
        auto tail = stmt_list();
        tail.push_back(out.assign(rebuilt(elements[count - 1]), out.local(stored)));
        for (auto k = count - 2; k >= 0; --k)
        {
            flat_stmt_id const then_body[] = {out.assign(rebuilt(elements[k]), out.local(stored))};
            auto const chain = out.if_(is_at(chosen, i32(k)), then_body, tail);
            tail.clear();
            tail.push_back(chain);
        }
        into.push_back_range(tail);
        return true;
    }

    stmt_list body(ast::range_of<flat_stmt_id> range, int depth) { return body(ids_of(out.e, range), depth); }

    stmt_list body(stmt_list const& ids, int depth)
    {
        auto result = stmt_list();
        for (auto const id : ids)
            stmt(id, result, depth);
        return result;
    }

    void stmt(flat_stmt_id id, stmt_list& into, int depth)
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
        {
            into.push_back(id);
            return;
        }
        auto s = out.e.at(id);
        if (auto const* const let = s.node.try_as<flat_let>(); let != nullptr && !parts_of(let->local).empty())
            return declare(s, let->local, let->value, into);
        if (auto const* const var = s.node.try_as<flat_var>(); var != nullptr && !parts_of(var->local).empty())
            return declare(s, var->local, var->value, into);
        if (auto const* const a = s.node.try_as<flat_assign>(); a != nullptr && assign(s, a->place, a->value, into))
            return;

        // a statement that holds statements is written again around its bodies, rewritten
        auto changed = false;
        auto const rewritten = [&](ast::range_of<flat_stmt_id> range)
        {
            changed = true;
            return out.stmt_list(body(range, depth + 1));
        };
        auto const arms = [&](ast::range_of<flat_arm> range)
        {
            auto list = cc::vector<flat_arm>();
            if (is_known(out.e, range))
                for (auto const& arm : out.e.at(range))
                    list.push_back(arm);
            for (auto& arm : list)
                arm.body = rewritten(arm.body);
            return out.arm_list(list);
        };
        if (auto* const n = s.node.try_as<flat_if>())
        {
            n->then_body = rewritten(n->then_body);
            n->else_body = rewritten(n->else_body);
        }
        else if (auto* const b = s.node.try_as<flat_block>())
            b->body = rewritten(b->body);
        else if (auto* const l = s.node.try_as<flat_loop>())
            l->body = rewritten(l->body);
        else if (auto* const w = s.node.try_as<flat_while>())
            w->body = rewritten(w->body);
        else if (auto* const f = s.node.try_as<flat_for>())
            f->body = rewritten(f->body);
        else if (auto* const o = s.node.try_as<flat_once>())
            o->body = rewritten(o->body);
        else if (auto* const k = s.node.try_as<flat_check>())
            k->body = rewritten(k->body);
        else if (auto* const c = s.node.try_as<flat_case>())
        {
            c->arms = arms(c->arms);
            c->default_body = rewritten(c->default_body);
        }
        else if (auto* const sw = s.node.try_as<flat_switch>())
        {
            sw->arms = arms(sw->arms);
            sw->default_body = rewritten(sw->default_body);
        }
        if (!changed)
        {
            into.push_back(id);
            return;
        }
        out.e.stmts.push_back(cc::move(s));
        into.push_back(flat_stmt_id(out.e.stmts.size() - 1));
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    /// A read of element `index` of the lowered `local`, as the node that takes the element's place.
    flat_expr read(flat_expr const& x, local_id local, flat_expr_id index)
    {
        auto const elements = cc::vector<local_id>(parts_of(local));
        auto const count = elements.size();
        at(x);
        if (auto const k = literal_index(index, count); k >= 0)
            return out.e.at(out.local(elements[k]));

        auto const label = out.add_label("selected");
        auto stmts = stmt_list();
        auto const chosen = out.add_local(local_kind::temporary, "chosen", out.e.at(index).type);
        stmts.push_back(out.let(chosen, index));
        // `select(chosen == 0, e0, select(chosen == 1, e1, e2))` where the element's type has a `select`
        auto choice = out.local(elements[count - 1]);
        for (auto k = count - 2; k >= 0 && is_valid(choice); --k)
        {
            flat_expr_id const arguments[] = {is_at(chosen, i32(k)), out.local(elements[k]), choice};
            choice = out.call("select", arguments);
            if (out.e.at(choice).type != x.type)
                choice = flat_expr_id::none;
        }
        if (is_valid(choice))
            stmts.push_back(out.leave(label, choice));
        else
        {
            // a struct has no `select`, so its element is left with under the test that picks it
            for (auto k = isize(0); k + 1 < count; ++k)
            {
                flat_stmt_id const then_body[] = {out.leave(label, out.local(elements[k]))};
                stmts.push_back(out.if_(is_at(chosen, i32(k)), then_body));
            }
            stmts.push_back(out.leave(label, out.local(elements[count - 1])));
        }
        auto const block = out.block_expr(label, x.type, stmts);
        return out.e.at(block);
    }

    void lower(stmt_list const& root_body)
    {
        auto const count = out.e.exprs.size();
        // first every statement list, whose places still name the lowered locals
        out.set_body(body(root_body, 0));
        for (auto i = isize(0); i < count; ++i)
            if (auto const* const b = out.e.exprs[i].node.try_as<flat_block>())
            {
                auto const range = b->body;
                auto const list = body(range, 1);
                auto const rewritten = out.stmt_list(list);
                out.e.exprs[i].node.as<flat_block>().body = rewritten;
            }
        // then each read of an element, and last each read of a whole lowered local
        for (auto i = isize(0); i < count; ++i)
        {
            auto const* const element = out.e.exprs[i].node.try_as<flat_element>();
            if (element == nullptr)
                continue;
            auto const local = lowered_ref(element->object);
            if (!is_valid(local))
                continue;
            auto const x = out.e.exprs[i];
            auto replaced = read(x, local, x.node.as<flat_element>().index);
            replaced.from = x.from;
            replaced.inlined_through = x.inlined_through;
            out.e.exprs[i] = cc::move(replaced);
        }
        for (auto i = isize(0); i < count; ++i)
        {
            auto const local = lowered_ref(flat_expr_id(i32(i)));
            if (!is_valid(local))
                continue;
            auto const x = out.e.exprs[i];
            at(x);
            auto references = cc::vector<flat_expr_id>();
            for (auto const p : parts_of(local))
                references.push_back(out.local(p));
            auto const whole = out.construct(x.type, references);
            out.e.exprs[i].node = out.e.at(whole).node;
        }
    }
};
} // namespace

bool sgl::check::impl::has_selected_arrays(checked_module const& m, flat_entry_point const& e)
{
    for (auto const& local : e.locals)
        if (is_lowered(m, e, local))
            return true;
    return false;
}

stmt_list sgl::check::impl::lower_selected_arrays(flat_builder& out, stmt_list const& body)
{
    if (!has_selected_arrays(out.m, out.e))
        return body;
    auto s = selector{.out = out, .parts = cc::vector<cc::vector<local_id>>::create_defaulted(out.e.locals.size())};
    // Internal error path: a place a builtin's `mut` parameter writes through has no choice among locals to stand for
    // it, and CHK-538 refuses one in the check pass, so only a tree built by hand reaches here; it keeps the array.
    // No builtin reaches it yet, since a builtin's parameter is never `is_mut`.
    auto written_through = cc::vector<bool>::create_filled(out.e.locals.size(), false);
    for (auto const& x : out.e.exprs)
    {
        auto const* const call = x.node.try_as<flat_call>();
        if (call == nullptr || !is_known(out.e, call->arguments) || !is_valid(call->callee))
            continue;
        auto const& symbol = out.m.at(call->callee);
        if (symbol.info < 0 || symbol.info >= out.m.functions.size())
            continue;
        auto const parameters = out.m.at(out.m.functions[symbol.info].parameters);
        auto const arguments = out.e.at(call->arguments);
        for (auto k = isize(0); k < parameters.size() && k < arguments.size(); ++k)
        {
            auto place = parameters[k].is_mut ? arguments[k] : flat_expr_id::none;
            for (auto depth = 0; is_known(out.e, place) && depth < k_max_depth; ++depth)
            {
                auto const& node = out.e.at(place).node;
                if (auto const* const ref = node.try_as<flat_local_ref>())
                {
                    if (is_known(out.e, ref->local))
                        written_through[index_of(ref->local)] = true;
                    break;
                }
                if (auto const* const member = node.try_as<flat_member>())
                    place = member->object;
                else if (auto const* const element = node.try_as<flat_element>())
                    place = element->object;
                else
                    break;
            }
        }
    }
    for (auto i = isize(0); i < s.parts.size(); ++i)
    {
        auto const local = out.e.locals[i];
        if (!is_lowered(out.m, out.e, local) || written_through[i])
            continue;
        auto const& t = out.m.at(local.type);
        for (auto k = 0; k < t.count; ++k)
        {
            auto const name = cc::format("{}_{}", local.source_name.empty() ? local.name : local.source_name, k);
            auto const part = out.add_local(local.is_mut ? local_kind::var : local_kind::let, name, t.element);
            auto& added = out.e.locals[index_of(part)];
            added.piece_of = local_id(i32(i));
            added.piece_member = k;
            added.is_undefined = local.is_undefined;
            added.scope_through = local.scope_through;
            s.parts[i].push_back(part);
        }
    }
    s.lower(body);
    return ids_of(out.e, out.e.body);
}
