#include "flat_builder.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::check;

namespace
{
flat_expr_id add_call(flat_builder& b, cc::string_view name, cc::span<flat_expr_id const> arguments, bool is_forced_effect)
{
    auto const list = b.expr_list(arguments);
    for (auto i = isize(0); i < b.m.symbols.size(); ++i)
    {
        auto const& s = b.m.symbols[i];
        if (s.kind != symbol_kind::function || !is_valid(s.intrinsic) || s.info < 0 || s.name != name)
            continue;
        auto const& f = b.m.functions[s.info];
        auto const parameters = b.m.at(f.parameters);
        auto is_match = parameters.size() == arguments.size();
        for (auto k = isize(0); is_match && k < parameters.size(); ++k)
            is_match = parameters[k].type == b.e.at(arguments[k]).type;
        if (!is_match)
            continue;
        return b.add_expr(f.result, flat_call{.callee = symbol_id(i),
                                              .intrinsic = s.intrinsic,
                                              .is_pure = f.is_pure && !is_forced_effect,
                                              .arguments = list});
    }
    return b.add_expr(checked_module::error_type, flat_invalid{});
}
} // namespace

flat_builder flat_builder::create(checked_module const& m, signature const& s)
{
    auto result = flat_builder{.m = m};
    result.e.entry_stage = s.entry_stage;
    result.e.name = s.name;
    result.e.input = s.input;
    result.e.result = s.result;

    result.e.names.reserve(s.name);
    for (auto const& other : m.symbols)
        result.e.names.reserve(other.name);

    result.add_local(local_kind::parameter, s.parameter_name, s.input);
    result.e.root = result.add_label("root");
    return result;
}

flat_builder flat_builder::extend(checked_module const& m, flat_entry_point e)
{
    return flat_builder{.m = m, .e = cc::move(e)};
}

type_id flat_builder::type_named(cc::string_view name) const
{
    for (auto const& s : m.symbols)
        // the prelude's symbols come first, so a struct of the user file that shadows one is never the one found
        if ((s.kind == symbol_kind::structure || s.kind == symbol_kind::enumeration) && s.name == name)
            return s.type;
    return type_id::none;
}

local_id flat_builder::add_local(local_kind kind, cc::string_view desired, type_id type)
{
    e.locals.push_back({.kind = kind, .name = e.names.mint(desired), .type = type, .is_mut = kind == local_kind::var});
    return local_id(e.locals.size() - 1);
}

label_id flat_builder::add_label(cc::string_view desired)
{
    auto const is_taken = [&](cc::string_view name)
    {
        for (auto const& l : e.labels)
            if (l.name == name)
                return true;
        return false;
    };
    auto name = cc::string(desired.empty() ? cc::string_view("block") : desired);
    for (auto i = 1; is_taken(name); ++i)
        name = cc::format("{}_{}", desired, i);
    e.labels.push_back({.name = cc::move(name)});
    return label_id(e.labels.size() - 1);
}

// ---- expressions ----------------------------------------------------------------------------------------------------

flat_expr_id flat_builder::literal(f64 value)
{
    return add_expr(type_named(builtins::k_float), flat_literal{.value = value});
}

flat_expr_id flat_builder::int_literal(i32 value)
{
    return add_expr(type_named(builtins::k_int), flat_int_literal{.value = value});
}

flat_expr_id flat_builder::bool_literal(bool value)
{
    return add_expr(type_named(builtins::k_bool), flat_bool_literal{.value = value});
}

flat_expr_id flat_builder::local(local_id id)
{
    return add_expr(e.at(id).type, flat_local_ref{.local = id});
}

flat_expr_id flat_builder::member(flat_expr_id object, i32 index)
{
    auto const members = m.at(m.at(e.at(object).type).members);
    auto const type = index >= 0 && index < members.size() ? members[index].type : checked_module::error_type;
    return add_expr(type, flat_member{.object = object, .member = index});
}

flat_expr_id flat_builder::member(flat_expr_id object, cc::string_view name)
{
    auto const members = m.at(m.at(e.at(object).type).members);
    for (auto i = isize(0); i < members.size(); ++i)
        if (members[i].name == name)
            return member(object, i32(i));
    return member(object, -1);
}

flat_expr_id flat_builder::construct(type_id type, cc::span<flat_expr_id const> arguments)
{
    return add_expr(type, flat_construct{.arguments = expr_list(arguments)});
}

flat_expr_id flat_builder::binding_member(symbol_id binding, i32 member)
{
    auto const members = m.at(m.bindings[m.at(binding).info].members);
    auto const type = member >= 0 && member < members.size() ? members[member].type : checked_module::error_type;
    return add_expr(type, flat_binding_member{.binding = binding, .member = member});
}

flat_expr_id flat_builder::buffer_element(flat_expr_id buffer, flat_expr_id index)
{
    return add_expr(m.at(e.at(buffer).type).element, flat_buffer_element{.buffer = buffer, .index = index});
}

flat_expr_id flat_builder::call(cc::string_view name, cc::span<flat_expr_id const> arguments)
{
    return add_call(*this, name, arguments, false);
}

flat_expr_id flat_builder::call_with_effect(cc::string_view name, cc::span<flat_expr_id const> arguments)
{
    return add_call(*this, name, arguments, true);
}

flat_expr_id flat_builder::not_(flat_expr_id operand)
{
    return add_expr(type_named(builtins::k_bool), flat_not{.operand = operand});
}

flat_expr_id flat_builder::and_(flat_expr_id lhs, flat_expr_id rhs)
{
    return add_expr(type_named(builtins::k_bool), flat_and{.lhs = lhs, .rhs = rhs});
}

flat_expr_id flat_builder::or_(flat_expr_id lhs, flat_expr_id rhs)
{
    return add_expr(type_named(builtins::k_bool), flat_or{.lhs = lhs, .rhs = rhs});
}

flat_expr_id flat_builder::block_expr(label_id label, type_id type, cc::span<flat_stmt_id const> body)
{
    return add_expr(type, flat_block{.label = label, .body = stmt_list(body)});
}

// ---- statements -----------------------------------------------------------------------------------------------------

flat_builder::declared flat_builder::let(cc::string_view desired, flat_expr_id value)
{
    auto const id = add_local(local_kind::let, desired, e.at(value).type);
    return {.local = id, .stmt = let(id, value)};
}

flat_stmt_id flat_builder::let(local_id local, flat_expr_id value)
{
    return add_stmt(flat_let{.local = local, .value = value});
}

flat_builder::declared flat_builder::var(cc::string_view desired, type_id type, flat_expr_id value)
{
    auto const id = add_local(local_kind::var, desired, type);
    return {.local = id, .stmt = var(id, value)};
}

flat_stmt_id flat_builder::var(local_id local, flat_expr_id value)
{
    return add_stmt(flat_var{.local = local, .value = value});
}

flat_stmt_id flat_builder::assign(flat_expr_id place, flat_expr_id value)
{
    return add_stmt(flat_assign{.place = place, .value = value});
}

flat_stmt_id flat_builder::print(flat_expr_id value)
{
    return add_stmt(flat_print{.value = value});
}

flat_stmt_id flat_builder::eval(flat_expr_id value)
{
    return add_stmt(flat_eval{.value = value});
}

flat_stmt_id flat_builder::if_(flat_expr_id condition,
                               cc::span<flat_stmt_id const> then_body,
                               cc::span<flat_stmt_id const> else_body)
{
    auto const then_list = stmt_list(then_body);
    auto const else_list = stmt_list(else_body);
    return add_stmt(flat_if{.condition = condition, .then_body = then_list, .else_body = else_list});
}

flat_stmt_id flat_builder::block(label_id label, cc::span<flat_stmt_id const> body)
{
    return add_stmt(flat_block{.label = label, .body = stmt_list(body)});
}

flat_stmt_id flat_builder::leave(label_id target, flat_expr_id value)
{
    return add_stmt(flat_leave{.target = target, .value = value});
}

flat_stmt_id flat_builder::loop(label_id label, cc::span<flat_stmt_id const> body)
{
    return add_stmt(flat_loop{.label = label, .body = stmt_list(body)});
}

flat_stmt_id flat_builder::while_(label_id label, flat_expr_id condition, cc::span<flat_stmt_id const> body)
{
    return add_stmt(flat_while{.label = label, .condition = condition, .body = stmt_list(body)});
}

flat_stmt_id flat_builder::for_(label_id label,
                                local_id index,
                                flat_expr_id first,
                                flat_expr_id end,
                                cc::span<flat_stmt_id const> body)
{
    return add_stmt(flat_for{.label = label, .index = index, .first = first, .end = end, .body = stmt_list(body)});
}

flat_stmt_id flat_builder::continue_(label_id target)
{
    return add_stmt(flat_continue{.target = target});
}

flat_stmt_id flat_builder::once(cc::span<flat_stmt_id const> body)
{
    return add_stmt(flat_once{.body = stmt_list(body)});
}

flat_stmt_id flat_builder::break_()
{
    return add_stmt(flat_break{});
}

flat_stmt_id flat_builder::return_(flat_expr_id value)
{
    return add_stmt(flat_return{.value = value});
}

ast::range_of<flat_expr_id> flat_builder::expr_list(cc::span<flat_expr_id const> list)
{
    auto const range = ast::range_of<flat_expr_id>{.first = u32(e.expr_lists.size()), .count = u32(list.size())};
    e.expr_lists.push_back_range(list);
    return range;
}

ast::range_of<flat_arm> flat_builder::arm_list(cc::span<flat_arm const> list)
{
    auto const range = ast::range_of<flat_arm>{.first = u32(e.arms.size()), .count = u32(list.size())};
    e.arms.push_back_range(list);
    return range;
}

ast::range_of<flat_stmt_id> flat_builder::stmt_list(cc::span<flat_stmt_id const> list)
{
    auto const range = ast::range_of<flat_stmt_id>{.first = u32(e.stmt_lists.size()), .count = u32(list.size())};
    e.stmt_lists.push_back_range(list);
    return range;
}

void flat_builder::set_body(cc::span<flat_stmt_id const> list)
{
    e.body = stmt_list(list);
}
