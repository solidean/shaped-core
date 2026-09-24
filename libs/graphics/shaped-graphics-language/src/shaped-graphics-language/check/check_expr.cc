#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto nothing_type = checked_module::nothing_type;

local_name const* find_local(function_scope const& scope, cc::string_view name)
{
    for (auto i = scope.locals.size() - 1; i >= 0; --i)
        if (scope.locals[i].name == name)
            return &scope.locals[i];
    return nullptr;
}
} // namespace

// ---- bodies ---------------------------------------------------------------------------------------------------------

void checker::check_body(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const& f = ast.at(out.at(id).declaration).node.as<ast::fun_decl>();
    auto const index = out.at(id).info;
    if (f.body.kind == ast::body_kind::none || notes[index].is_body_checked)
        return;
    notes[index].is_body_checked = true;

    // by value: checking the body may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    auto scope = function_scope{.function = id, .file = file, .result = info.result};
    for (auto const& p : out.at(info.parameters))
    {
        if (names.contains(p.name))
            unsupported(file, ast.at(p.field).name, "a parameter that shadows a module-level name");
        scope.locals.push_back({
            .name = text_of(file, ast.at(p.field).name),
            .where = {.kind = target_kind::parameter, .index = i32(p.field)},
            .type = p.type,
        });
    }

    auto const errors_before = error_count();

    auto ending = check_statements(scope, f.body.statements);
    if (ast::is_valid(f.body.value) && notes[index].infers_result)
    {
        auto type = check_expr(scope, f.body.value);
        if (type == nothing_type)
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, f.body.value),
                   cc::format("the body of {} is its result, and this is nothing", out.at(id).name));
            type = error_type;
        }
        out.functions[index].result = type;
        ending = flow::exits;
    }
    else if (ast::is_valid(f.body.value))
    {
        check_return(scope, span_of(file, f.body.value), f.body.value);
        ending = flow::exits;
    }
    // a block without a statement was reported where it was parsed
    auto const has_statements = !f.body.statements.empty() || ast::is_valid(f.body.value);
    if (has_statements && ending == flow::falls_through && info.result != nothing_type && info.result != error_type)
        report(diagnostic_kind::missing_return, file, f.name,
               cc::format("{} returns {}, and a path through its body ends without a return", out.at(id).name,
                          out.name_of(info.result)));

    notes[index].is_body_sound = error_count() == errors_before;
}

type_id checker::check_index(function_scope& scope, ast::expr_id id, ast::index const& node)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);

    auto const outer = subscripted;
    subscripted = node.object;
    auto const object = check_expr(scope, node.object);
    subscripted = outer;
    if (object == error_type)
        return error_type;
    if (out.at(object).kind != type_kind::buffer)
    {
        unsupported(file, where, "a subscript on anything but a buffer");
        return error_type;
    }

    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where, "a buffer takes one index: `values[i]`");
        return error_type;
    }

    auto const index = check_expr(scope, arguments[0].value);
    auto const int_type = type_of_builtin(builtins::k_int, file, where);
    if (index != error_type && index != int_type)
        report(diagnostic_kind::type_mismatch, file, span_of(file, arguments[0].value),
               cc::format("a buffer is indexed by an int, and this is a {}", out.name_of(index)));
    return out.at(object).element;
}

void checker::convert_object(function_scope& scope, ast::expr_id object, type_id to)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, object);
    auto const elements = ast.at(ast.at(object).node.as<ast::object>().elements);
    set_type(file, object, to);

    auto const target_type = out.at(to);
    auto const members = out.at(target_type.members);
    if (to != error_type && target_type.is_opaque)
        report(diagnostic_kind::type_mismatch, file, where,
               cc::format("{} has no fields an object could name", out.name_of(to)));

    auto seen = cc::vector<bool>::create_filled(members.size(), false);
    for (auto const& e : elements)
    {
        auto const element_where = span_of(file, e.form);
        judge_attributes(file, e.attributes, {}, "an object element");
        if (e.is_splat)
        {
            unsupported(file, element_where, "a splat in an object");
            continue;
        }
        if (e.is_shorthand)
        {
            unsupported(file, element_where, "a shorthand object element");
            continue;
        }

        auto member = isize(-1);
        auto const name = text_of(file, e.name);
        for (auto i = isize(0); i < members.size(); ++i)
            if (members[i].name == name)
                member = i;

        // a positional element was reported by the AST pass
        if (member < 0 && !e.name.empty() && to != error_type && !target_type.is_opaque)
            report(diagnostic_kind::unknown_field, file, e.name, cc::format("{} has no field {}", out.name_of(to), name));
        if (member >= 0 && seen[member])
            report(diagnostic_kind::duplicate_field, file, e.name, name);
        if (member >= 0)
            seen[member] = true;

        auto const expected = member >= 0 ? members[member].type : error_type;
        if (ast::is_valid(e.value) && ast.at(e.value).node.is<ast::object>())
        {
            convert_object(scope, e.value, expected);
            continue;
        }
        auto const type = check_expr(scope, e.value);
        if (type != error_type && expected != error_type && type != expected)
            report(diagnostic_kind::type_mismatch, file, span_of(file, e.value),
                   cc::format("{} is {}, got {}", name, out.name_of(expected), out.name_of(type)));
    }

    for (auto i = isize(0); i < members.size(); ++i)
        if (!seen[i])
            report(diagnostic_kind::missing_field, file, where, members[i].name);
}

// ---- expressions ----------------------------------------------------------------------------------------------------

type_id checker::check_cast(function_scope& scope, ast::expr_id id, ast::cast const& node)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const from = check_expr(scope, node.value);
    auto const to = resolve_value_type(file, node.type);
    if (from == error_type || to == error_type)
        return error_type;
    if (from == to)
        return to;

    // Overloads of `as` differ in their result as well, so the one that matches both ends is the conversion.
    auto const* const candidates = operators.get_ptr("as");
    if (candidates != nullptr)
        for (auto const candidate : *candidates)
        {
            if (demand(candidate, file, where) != symbol_state::checked)
                continue;
            auto const& info = out.functions[out.at(candidate).info];
            auto const parameters = out.at(info.parameters);
            if (parameters.size() != 1 || parameters[0].type != from || info.result != to)
                continue;
            set_target(file, id, {.kind = target_kind::overload, .symbol = candidate});
            return to;
        }
    report(diagnostic_kind::no_matching_overload, file, where,
           cc::format("{} as {}: no conversion", out.name_of(from), out.name_of(to)));
    return error_type;
}

type_id checker::check_expr(function_scope& scope, ast::expr_id expr)
{
    if (!ast::is_valid(expr))
        return error_type;

    auto const file = scope.file;
    auto const& e = ast_of(file).at(expr);
    auto const where = span_of(file, expr);
    judge_attributes(file, e.attributes, {}, "an expression");

    auto const not_yet = [&](cc::string_view construct)
    {
        unsupported(file, where, construct);
        return error_type;
    };

    auto const type = e.node.visit(
        [&](ast::invalid_expr const&) { return error_type; }, [&](ast::literal const& l)
        { return check_literal(scope, expr, l); }, [&](ast::name const& n) { return check_name(scope, expr, n); },
        [&](ast::member const& m) { return check_member(scope, expr, m); }, [&](ast::call const& c)
        { return check_call(scope, expr, c); }, [&](ast::self_ref const&) { return not_yet("self"); },
        [&](ast::wildcard const&) { return not_yet("a wildcard as a value"); },
        // CHK-152: a leading dot needs a type the context expects, which today only a `case` pattern gives it
        [&](ast::leading_dot const&) { return not_yet("a leading-dot name outside a case pattern"); },
        [&](ast::index const& node) { return check_index(scope, expr, node); },
        // AST-128: `mut buffer[float]` and its neighbours parse, and the binding model they belong to is unbuilt.
        [&](ast::qualified_type const&) { return not_yet("a resource type"); },
        [&](ast::tuple const&) { return not_yet("a tuple"); }, [&](ast::array const&) { return not_yet("an array"); },
        [&](ast::object const&) { return not_yet("an object with no struct to convert to"); },
        [&](ast::comparison_chain const& chain) { return check_chain(scope, expr, chain); }, [&](ast::cast const& node)
        { return check_cast(scope, expr, node); }, [&](ast::membership const&) { return not_yet("in"); },
        [&](ast::ascription const&) { return not_yet("a type ascription"); },
        [&](ast::range const&) { return not_yet("a range"); }, [&](ast::lambda const&) { return not_yet("a lambda"); },
        [&](ast::case_expr const& c) { return check_case(scope, expr, c, true); },
        [&](ast::loop_expr const& loop)
        {
            auto has_break = false;
            return check_loop(scope, expr, loop, true, has_break);
        },
        [&](ast::return_expr const&) { return not_yet("return as a value"); }, [&](ast::yield_expr const&)
        { return not_yet("yield as a value"); }, [&](ast::break_expr const&) { return not_yet("break as a value"); },
        [&](ast::continue_expr const&) { return not_yet("continue as a value"); }, [&](ast::struct_type const&)
        { return not_yet("a type as a value"); }, [&](ast::function_type const&) { return not_yet("a type as a value"); },
        // reserved, and reported by the AST pass
        [&](ast::with_bindings const&) { return error_type; });

    set_type(file, expr, type);
    return type;
}

type_id checker::check_literal(function_scope& scope, ast::expr_id id, ast::literal const& literal)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    if (literal.kind == ast::literal_kind::quoted)
    {
        unsupported(file, where, "a string literal");
        return error_type;
    }
    if (literal.kind == ast::literal_kind::hash)
    {
        unsupported(file, where, "a hash literal");
        return error_type;
    }

    auto const text = text_of(file, where);
    switch (classify_number(text))
    {
    case number_class::plain_integer:
        if (!parse_plain_integer(text).has_value())
        {
            unsupported(file, where, "an integer literal that does not fit an int");
            return error_type;
        }
        return type_of_builtin(builtins::k_int, file, where);
    case number_class::other:
        unsupported(file, where, "a number literal with a prefix, a suffix or a p exponent");
        return error_type;
    case number_class::plain_float:
        break;
    }
    if (!parse_plain_float(text).has_value())
    {
        unsupported(file, where, "a float literal this large");
        return error_type;
    }
    return type_of_builtin(builtins::k_float, file, where);
}

type_id checker::check_name(function_scope& scope, ast::expr_id id, ast::name const& name)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const text = text_of(file, name.where);

    if (auto const* const local = find_local(scope, text))
    {
        set_target(file, id, local->where);
        return local->type;
    }

    auto const* const found = names.get_ptr(text);
    if (found == nullptr || found->empty())
    {
        report(diagnostic_kind::unknown_name, file, where, text);
        return error_type;
    }

    auto const symbol = found->front();
    set_target(file, id, {.kind = target_kind::symbol, .symbol = symbol});
    switch (out.at(symbol).kind)
    {
    case symbol_kind::structure:
    case symbol_kind::enumeration:
        unsupported(file, where, "a type as a value");
        break;
    case symbol_kind::function:
        unsupported(file, where, "a function as a value");
        break;
    case symbol_kind::binding:
        unsupported(file, where, "a binding as a value");
        break;
    case symbol_kind::unsupported:
        break;
    }
    return error_type;
}

type_id checker::check_member(function_scope& scope, ast::expr_id id, ast::member const& member)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const name = text_of(file, member.name);

    auto const find_member = [&](ast::range_of<member_info> range)
    {
        auto const members = out.at(range);
        for (auto i = isize(0); i < members.size(); ++i)
            if (members[i].name == name)
                return i;
        return isize(-1);
    };

    // `constants.view_projection`: a binding is no value, so the object is looked at before it is checked
    auto const* const object_name
        = ast::is_valid(member.object) ? ast.at(member.object).node.try_as<ast::name>() : nullptr;
    if (object_name != nullptr && find_local(scope, text_of(file, object_name->where)) == nullptr)
    {
        auto const* const found = names.get_ptr(text_of(file, object_name->where));
        if (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::binding)
        {
            auto const binding = found->front();
            auto const object_where = span_of(file, member.object);
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = binding});
            if (demand(binding, file, object_where) != symbol_state::checked)
                return error_type;

            auto is_listed = false;
            for (auto const listed : out.at(out.functions[out.at(scope.function).info].bindings))
                is_listed = is_listed || listed == binding;
            if (!is_listed)
                report(diagnostic_kind::binding_not_listed, file, object_where,
                       cc::format("{} is not in the binding list of {}", out.at(binding).name,
                                  out.at(scope.function).name));

            auto const index = find_member(out.bindings[out.at(binding).info].members);
            if (index < 0)
            {
                if (!member.name.empty())
                    report(diagnostic_kind::unknown_member, file, member.name,
                           cc::format("the binding {} has no member {}", out.at(binding).name, name));
                return error_type;
            }
            set_target(file, id, {.kind = target_kind::binding_member, .symbol = binding, .index = i32(index)});
            auto const type = out.at(out.bindings[out.at(binding).info].members)[index].type;
            if (type != error_type && out.at(type).kind == type_kind::buffer && id != subscripted)
            {
                unsupported(file, span_of(file, id), "a buffer as a value; read an element of it, as in `values[i]`");
                return error_type;
            }
            return type;
        }

        // `light_kind.point`: an enum is no value either (CHK-148), so its name never reaches `check_expr`
        if (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::enumeration)
        {
            auto const enumeration = found->front();
            auto const object_where = span_of(file, member.object);
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = enumeration});
            if (demand(enumeration, file, object_where) != symbol_state::checked)
                return error_type;

            auto const type = out.at(enumeration).type;
            auto const cases = out.at(out.at(type).cases);
            auto index = isize(-1);
            for (auto i = isize(0); i < cases.size(); ++i)
                if (cases[i].name == name)
                    index = i;
            if (index < 0)
            {
                if (!member.name.empty())
                    report(diagnostic_kind::unknown_member, file, member.name,
                           cc::format("the enum {} has no case {}", out.at(enumeration).name, name));
                return error_type;
            }
            set_target(file, id, {.kind = target_kind::enum_case, .symbol = enumeration, .index = i32(index)});
            return type;
        }
    }

    auto const object = check_expr(scope, member.object);
    if (object == error_type || member.name.empty())
        return error_type;

    auto const& type = out.at(object);
    auto const index = find_member(type.members);
    if (index < 0)
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}", out.name_of(object), name));
        return error_type;
    }
    set_target(file, id, {.kind = target_kind::field, .symbol = type.symbol, .index = i32(index)});
    return out.at(type.members)[index].type;
}

// ---- calls ----------------------------------------------------------------------------------------------------------

call_arguments checker::check_arguments(function_scope& scope, ast::range_of<ast::argument> range, bool is_constructor)
{
    auto const file = scope.file;
    auto result = call_arguments();

    for (auto const& a : ast_of(file).at(range))
    {
        auto const where = span_of(file, a.form);
        judge_attributes(file, a.attributes, {}, "an argument");
        if (!a.attributes.empty())
            result.is_poisoned = true;
        if (!a.name.empty())
        {
            unsupported(file, where, "a named argument");
            result.is_poisoned = true;
        }

        auto const type = check_expr(scope, a.value);
        if (type == error_type)
        {
            result.is_poisoned = true;
            continue;
        }
        if (!a.is_splat)
        {
            result.types.push_back(type);
            continue;
        }

        if (!is_constructor)
        {
            unsupported(file, where, "a splat outside a constructor call");
            result.is_poisoned = true;
        }
        else if (out.at(type).is_opaque)
        {
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("{} has no fields a splat could spread", out.name_of(type)));
            result.is_poisoned = true;
        }
        else
            for (auto const& m : out.at(out.at(type).members))
            {
                result.types.push_back(m.type);
                result.is_poisoned = result.is_poisoned || m.type == error_type;
            }
    }
    return result;
}

cc::string checker::signature_text(cc::string_view spelling, cc::span<type_id const> types) const
{
    auto text = cc::string(spelling);
    text += "(";
    for (auto i = isize(0); i < types.size(); ++i)
    {
        if (i > 0)
            text += ", ";
        text += out.name_of(types[i]);
    }
    text += ")";
    return text;
}

type_id checker::check_call(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);

    if (sgl::is_valid(call.op))
    {
        auto const spelling = text_of(file, file_of(file).at(call.op).where);
        // `and`, `or` and `not` are the language's own: no function could leave an operand unevaluated
        if (call.is_short_circuit || spelling == "not")
            return check_logical(scope, id, call);
        auto arguments = check_arguments(scope, call.arguments, false);
        // `==` and `!=` over one enum are the language's own (CHK-149), and what they compare is the cases'
        // `int`s (EVAL-64) — so they resolve to the `int` overload, and no later pass needs an enum rule.
        if ((spelling == "==" || spelling == "!=") && arguments.types.size() == 2
            && arguments.types[0] == arguments.types[1] && out.at(arguments.types[0]).kind == type_kind::enumeration)
        {
            auto const as_int = type_of_builtin(builtins::k_int, file, where);
            arguments.types[0] = as_int;
            arguments.types[1] = as_int;
        }
        auto const* const found = operators.get_ptr(spelling);
        auto const none = cc::span<symbol_id const>();
        return resolve_overload(scope, id, ast::expr_id::none,
                                found != nullptr ? cc::span<symbol_id const>(*found) : none, arguments,
                                cc::format("operator {}", spelling));
    }

    if (!ast::is_valid(call.callee))
        return error_type;

    auto const& callee = ast.at(call.callee);
    auto const callee_where = span_of(file, call.callee);
    auto const* const n = callee.node.try_as<ast::name>();
    if (n == nullptr)
    {
        (void)check_arguments(scope, call.arguments, false);
        if (callee.node.is<ast::member>())
            unsupported(file, callee_where, "a method call");
        else if (callee.node.is<ast::index>())
            unsupported(file, callee_where, "type arguments");
        else if (!callee.node.is<ast::invalid_expr>())
            unsupported(file, callee_where, "a call of something that is no name");
        return error_type;
    }

    auto const text = text_of(file, n->where);
    if (auto const* const local = find_local(scope, text))
    {
        set_target(file, call.callee, local->where);
        (void)check_arguments(scope, call.arguments, false);
        unsupported(file, callee_where, "a call of a local value");
        return error_type;
    }

    auto const* const found = names.get_ptr(text);
    if (found == nullptr || found->empty())
    {
        (void)check_arguments(scope, call.arguments, false);
        report(diagnostic_kind::unknown_name, file, callee_where, text);
        return error_type;
    }

    auto const first = found->front();
    switch (out.at(first).kind)
    {
    case symbol_kind::structure:
        return construct(scope, id, call.callee, first, check_arguments(scope, call.arguments, true));
    case symbol_kind::function:
        return resolve_overload(scope, id, call.callee, *found, check_arguments(scope, call.arguments, false), text);
    case symbol_kind::binding:
        (void)check_arguments(scope, call.arguments, false);
        set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
        report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
               cc::format("{} is a binding, and a call needs a function or a struct", text));
        return error_type;
    case symbol_kind::unsupported:
        (void)check_arguments(scope, call.arguments, false);
        return error_type;
    }
    return error_type;
}

type_id checker::construct(function_scope& scope,
                           ast::expr_id id,
                           ast::expr_id callee,
                           symbol_id structure,
                           call_arguments const& arguments)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    if (demand(structure, file, where) != symbol_state::checked)
        return error_type;

    auto const type = out.at(structure).type;
    auto const self = target{.kind = target_kind::constructor, .symbol = structure};
    set_target(file, id, self);
    set_target(file, callee, self);
    set_type(file, callee, type);

    // The result is known whatever the arguments are, so one bad argument does not take the whole value with it.
    if (arguments.is_poisoned)
        return type;

    auto const members = out.at(out.at(type).members);
    auto is_match = !out.at(type).is_opaque && members.size() == arguments.types.size();
    auto is_silent = false;
    for (auto i = isize(0); is_match && i < members.size(); ++i)
    {
        is_silent = is_silent || members[i].type == error_type;
        is_match = members[i].type == error_type || members[i].type == arguments.types[i];
    }
    if (!is_match && !is_silent)
    {
        auto expected = cc::vector<type_id>();
        for (auto const& m : members)
            expected.push_back(m.type);
        auto const name = out.name_of(type);
        report(diagnostic_kind::no_matching_overload, file, where,
               out.at(type).is_opaque ? cc::format("{} is opaque and has no constructor", name)
                                      : cc::format("{}, and the constructor is {}", signature_text(name, arguments.types),
                                                   signature_text(name, expected)));
    }
    return type;
}

type_id checker::resolve_overload(function_scope& scope,
                                  ast::expr_id id,
                                  ast::expr_id callee,
                                  cc::span<symbol_id const> candidates,
                                  call_arguments const& arguments,
                                  cc::string_view spelling)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);

    auto is_silent = arguments.is_poisoned;
    auto matches = cc::vector<symbol_id>();
    for (auto const candidate : candidates)
    {
        if (is_out_of_the_running(candidate, arguments.types))
            continue;
        if (demand(candidate, file, where) != symbol_state::checked)
        {
            is_silent = true;
            continue;
        }
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        auto is_match = !arguments.is_poisoned && parameters.size() == arguments.types.size();
        for (auto i = isize(0); is_match && i < parameters.size(); ++i)
            is_match = parameters[i].type == arguments.types[i];
        if (is_match)
            matches.push_back(candidate);
    }

    if (matches.empty())
    {
        if (!is_silent)
            report(diagnostic_kind::no_matching_overload, file, where, signature_text(spelling, arguments.types));
        return error_type;
    }
    if (matches.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where,
               cc::format("{} has {} candidates", signature_text(spelling, arguments.types), matches.size()));
        return error_type;
    }

    auto const chosen = matches.front();
    auto const self = target{.kind = target_kind::overload, .symbol = chosen};
    set_target(file, id, self);
    set_target(file, callee, self);
    if (is_valid(out.at(chosen).intrinsic))
        return out.functions[out.at(chosen).info].result;

    // A call of a function of the program is inlined, so it is an edge recursion is looked for along.
    calls.push_back({.caller = scope.function, .callee = chosen, .file = file, .where = where});

    // Bindings are an effect: what the callee reads, the caller has to list, and so on up to the entry point.
    auto const listed = out.at(out.functions[out.at(scope.function).info].bindings);
    for (auto const needed : out.at(out.functions[out.at(chosen).info].bindings))
    {
        auto is_listed = false;
        for (auto const l : listed)
            is_listed = is_listed || l == needed;
        if (!is_listed)
            report(diagnostic_kind::binding_not_listed, file, where,
                   cc::format("{} needs {}, which is not in the binding list of {}", out.at(chosen).name,
                              out.at(needed).name, out.at(scope.function).name));
    }
    return out.functions[out.at(chosen).info].result;
}

type_id checker::check_logical(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const spelling = text_of(file, file_of(file).at(call.op).where);
    auto const arguments = check_arguments(scope, call.arguments, false);
    if (arguments.is_poisoned)
        return error_type;

    auto const bool_type = type_of_builtin(builtins::k_bool, file, where);
    auto is_match = arguments.types.size() == (spelling == "not" ? 1 : 2);
    for (auto const type : arguments.types)
        is_match = is_match && type == bool_type;
    if (!is_match && bool_type != error_type)
        report(diagnostic_kind::no_matching_overload, file, where,
               signature_text(cc::format("operator {}", spelling), arguments.types));
    return bool_type;
}

type_id checker::check_chain(function_scope& scope, ast::expr_id id, ast::comparison_chain const& chain)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);
    auto const operands = ast.at(chain.operands);
    auto const operators = ast.at(chain.operators);

    auto types = cc::vector<type_id>();
    for (auto const operand : operands)
        types.push_back(check_expr(scope, operand));

    auto const bool_type = type_of_builtin(builtins::k_bool, file, where);
    for (auto i = isize(0); i < operators.size() && i + 1 < types.size(); ++i)
    {
        if (types[i] == error_type || types[i + 1] == error_type)
            continue;
        type_id const pair[] = {types[i], types[i + 1]};
        auto const spelling = text_of(file, file_of(file).at(operators[i]).where);
        auto const chosen = resolve_operator(file, where, spelling, pair);
        if (is_valid(chosen) && out.functions[out.at(chosen).info].result != bool_type && bool_type != error_type)
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("a comparison in a chain is a bool, and operator {} gives {}", spelling,
                              out.name_of(out.functions[out.at(chosen).info].result)));
    }
    return bool_type;
}

bool checker::is_out_of_the_running(symbol_id candidate, cc::span<type_id const> types) const
{
    auto const& s = out.at(candidate);
    if (s.kind != symbol_kind::function || s.state != symbol_state::in_compilation || s.info < 0)
        return false;
    auto const parameters = out.at(out.functions[s.info].parameters);
    auto is_match = parameters.size() == types.size();
    for (auto i = isize(0); is_match && i < parameters.size(); ++i)
        is_match = parameters[i].type == types[i];
    return !is_match;
}

symbol_id checker::find_operator(cc::string_view spelling, cc::span<type_id const> types) const
{
    auto const* const found = operators.get_ptr(spelling);
    if (found == nullptr)
        return symbol_id::none;
    auto result = symbol_id::none;
    for (auto const candidate : *found)
    {
        if (out.at(candidate).state != symbol_state::checked)
            continue;
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        auto is_match = parameters.size() == types.size();
        for (auto i = isize(0); is_match && i < parameters.size(); ++i)
            is_match = parameters[i].type == types[i];
        if (is_match && is_valid(result))
            return symbol_id::none;
        if (is_match)
            result = candidate;
    }
    return result;
}

symbol_id checker::resolve_operator(i32 file, source_span where, cc::string_view spelling, cc::span<type_id const> types)
{
    auto matches = 0;
    auto is_silent = false;
    auto chosen = symbol_id::none;
    if (auto const* const found = operators.get_ptr(spelling))
        for (auto const candidate : *found)
        {
            if (is_out_of_the_running(candidate, types))
                continue;
            if (demand(candidate, file, where) != symbol_state::checked)
            {
                is_silent = true;
                continue;
            }
            auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
            auto is_match = parameters.size() == types.size();
            for (auto i = isize(0); is_match && i < parameters.size(); ++i)
                is_match = parameters[i].type == types[i];
            if (is_match)
            {
                ++matches;
                chosen = candidate;
            }
        }

    auto const text = signature_text(cc::format("operator {}", spelling), types);
    if (matches == 0)
    {
        if (!is_silent)
            report(diagnostic_kind::no_matching_overload, file, where, text);
        return symbol_id::none;
    }
    if (matches > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where, cc::format("{} has {} candidates", text, matches));
        return symbol_id::none;
    }
    if (!is_valid(out.at(chosen).intrinsic))
    {
        unsupported(file, where, "an operator of the program's own where no call is written");
        return symbol_id::none;
    }
    return chosen;
}
