#include <clean-core/common/utility.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// What no target writes, erased: rule V0 (LEGAL-53), checks and asserts, first, and rule V1 (LEGAL-52), void, last.
// Void has one value and no representation, so what a void value did is kept as an effect, and the value itself is gone.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
struct void_eraser
{
    flat_builder& out;
    /// False for rule V0, which drops the checks and changes nothing else.
    bool erases_void = true;

    [[nodiscard]] bool is_void(flat_expr_id id) const
    {
        return is_known(out.e, id) && out.e.at(id).type == checked_module::void_type;
    }

    [[nodiscard]] bool is_void(local_id id) const
    {
        return is_known(out.e, id) && out.e.at(id).type == checked_module::void_type;
    }

    /// The value of void, attributed to what `from` was attributed to.
    flat_expr_id unit(flat_expr const& from)
    {
        out.from = from.from;
        out.inlined_through = from.inlined_through;
        return out.add_expr(checked_module::void_type, flat_construct{});
    }

    /// `id` with every read of a void local, and every void field of a value without an effect, as void's value.
    flat_expr_id expr(flat_expr_id id, int depth)
    {
        if (!is_known(out.e, id) || depth > k_max_depth || !erases_void)
            return id;
        auto copy = out.e.at(id);
        if (copy.type == checked_module::void_type && copy.node.is<flat_local_ref>())
            return unit(copy);
        if (auto* const member = copy.node.try_as<flat_member>())
        {
            if (copy.type == checked_module::void_type && !has_effect(out.e, member->object))
                return unit(copy);
            member->object = expr(member->object, depth + 1);
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
        else if (auto* const element = copy.node.try_as<flat_buffer_element>())
        {
            element->buffer = expr(element->buffer, depth + 1);
            element->index = expr(element->index, depth + 1);
        }
        else
            return id;
        out.e.exprs.push_back(cc::move(copy));
        return flat_expr_id(out.e.exprs.size() - 1);
    }

    ast::range_of<flat_expr_id> exprs(ast::range_of<flat_expr_id> range, int depth)
    {
        auto list = cc::vector<flat_expr_id>();
        if (is_known(out.e, range))
            for (auto const id : out.e.at(range))
                list.push_back(id);
        for (auto& id : list)
            id = expr(id, depth + 1);
        return out.expr_list(list);
    }

    /// Keeps what evaluating a void `value` does, and nothing when it does nothing.
    void effect_of(flat_stmt const& s, flat_expr_id value, stmt_list& into)
    {
        if (!is_valid(value) || !has_effect(out.e, value))
            return;
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        into.push_back(out.eval(expr(value, 0)));
    }

    ast::range_of<flat_stmt_id> body(ast::range_of<flat_stmt_id> range, int depth)
    {
        auto list = stmt_list();
        if (is_known(out.e, range))
            for (auto const id : out.e.at(range))
                list.push_back(id);
        return out.stmt_list(erase(list, depth + 1));
    }

    ast::range_of<flat_arm> arms(ast::range_of<flat_arm> range, int depth)
    {
        auto list = cc::vector<flat_arm>();
        if (is_known(out.e, range))
            for (auto const& arm : out.e.at(range))
                list.push_back(arm);
        for (auto& arm : list)
            arm.body = body(arm.body, depth);
        return out.arm_list(list);
    }

    /// V1 for one statement that declares, assigns, drops or returns a void value; false for any other.
    bool erase_void_statement(flat_stmt& s, stmt_list& into)
    {
        auto value = flat_expr_id::none;
        if (auto const* const let = s.node.try_as<flat_let>(); let != nullptr && is_void(let->local))
            value = let->value;
        else if (auto const* const var = s.node.try_as<flat_var>(); var != nullptr && is_void(var->local))
            value = var->value;
        else if (auto const* const assign = s.node.try_as<flat_assign>(); assign != nullptr && is_void(assign->value))
            value = assign->value;
        else if (auto const* const eval = s.node.try_as<flat_eval>();
                 eval != nullptr && is_void(eval->value) && !out.e.at(eval->value).node.is<flat_call>())
            value = eval->value;
        else if (auto const* const ret = s.node.try_as<flat_return>(); ret != nullptr && is_void(ret->value))
        {
            effect_of(s, ret->value, into);
            s.node = flat_return{};
            out.e.stmts.push_back(cc::move(s));
            into.push_back(flat_stmt_id(out.e.stmts.size() - 1));
            return true;
        }
        else
            return false;
        effect_of(s, value, into);
        return true;
    }

    stmt_list erase(stmt_list const& list, int depth)
    {
        auto result = stmt_list();
        for (auto const id : list)
        {
            if (!is_known(out.e, id) || depth > k_max_depth)
            {
                result.push_back(id);
                continue;
            }
            auto copy = out.e.at(id);
            // V0: a check and its body are gone, whatever they compute
            if (copy.node.is<flat_check>())
                continue;
            if (erases_void && erase_void_statement(copy, result))
                continue;

            copy.node.visit([&](flat_let& n) { n.value = expr(n.value, 0); }, //
                            [&](flat_var& n) { n.value = expr(n.value, 0); },
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
                                n.then_body = body(n.then_body, depth);
                                n.else_body = body(n.else_body, depth);
                            },
                            [&](flat_block& n) { n.body = body(n.body, depth); }, [&](flat_leave& n)
                            { n.value = expr(n.value, 0); }, [&](flat_loop& n) { n.body = body(n.body, depth); },
                            [&](flat_while& n)
                            {
                                n.condition = expr(n.condition, 0);
                                n.body = body(n.body, depth);
                            },
                            [&](flat_for& n)
                            {
                                n.first = expr(n.first, 0);
                                n.end = expr(n.end, 0);
                                n.body = body(n.body, depth);
                            },
                            [&](flat_continue&) {}, //
                            [&](flat_once& n) { n.body = body(n.body, depth); }, [&](flat_break&) {},
                            [&](flat_case& n)
                            {
                                n.scrutinee = expr(n.scrutinee, 0);
                                n.arms = arms(n.arms, depth);
                                n.default_body = body(n.default_body, depth);
                            },
                            [&](flat_switch& n)
                            {
                                n.scrutinee = expr(n.scrutinee, 0);
                                n.arms = arms(n.arms, depth);
                                n.default_body = body(n.default_body, depth);
                            },
                            [&](flat_return& n) { n.value = expr(n.value, 0); },
                            // dropped above
                            [&](flat_check&) {});
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return result;
    }
};
} // namespace

stmt_list sgl::check::impl::erase_checks(flat_builder& out, stmt_list const& body)
{
    return void_eraser{.out = out, .erases_void = false}.erase(body, 0);
}

stmt_list sgl::check::impl::erase_void(flat_builder& out, stmt_list const& body)
{
    return void_eraser{.out = out}.erase(body, 0);
}
