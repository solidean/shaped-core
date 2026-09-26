#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;
} // namespace

// ---- bodies ---------------------------------------------------------------------------------------------------------

void checker::check_body(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const index = out.at(id).info;
    if (notes[index].is_body_checked)
        return;
    auto const& f = ast.at(out.at(id).declaration).node.as<ast::fun_decl>();
    if (f.body.kind == ast::body_kind::none)
        return;
    notes[index].is_body_checked = true;

    // by value: checking the body may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    auto scope = function_scope{.function = id, .file = file, .result = info.result};
    for (auto const& p : out.at(info.parameters))
    {
        // CHK-54: a parameter may have the name of a module-level symbol, which it hides in the body.
        judge_shadowing(file, text_of(file, ast.at(p.field).name), ast.at(p.field).name);
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
        out.functions[index].result = check_expr(scope, f.body.value);
        ending = flow::exits;
    }
    else if (ast::is_valid(f.body.value))
    {
        check_return(scope, span_of(file, f.body.value), f.body.value);
        ending = flow::exits;
    }
    // a block without a statement was reported where it was parsed
    auto const has_statements = !f.body.statements.empty() || ast::is_valid(f.body.value);
    if (has_statements && ending == flow::falls_through && info.result != void_type && info.result != error_type)
        report(diagnostic_kind::missing_return, file, f.name,
               cc::format("{} returns {}, and a path through its body ends without a return", out.at(id).name,
                          out.name_of(info.result)));

    notes[index].is_body_sound = error_count() == errors_before;
}

void checker::check_defaults(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const index = out.at(id).info;
    // by value: checking a default may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    auto scope = function_scope{.function = id, .file = file, .result = info.result};
    auto const errors_before = error_count();
    for (auto const& p : out.at(info.parameters))
    {
        if (ast::is_valid(p.field) && p.has_default)
        {
            auto const value = ast.at(p.field).default_value;
            auto const type = check_expr(scope, value);
            if (type != error_type && p.type != error_type && type != p.type)
                report(diagnostic_kind::type_mismatch, file, span_of(file, value),
                       cc::format("the default of {} is {}, and {} takes {}", p.name, out.name_of(type), p.name,
                                  out.name_of(p.type)));
        }
        // a parameter is visible to the defaults after it
        if (ast::is_valid(p.field))
            scope.locals.push_back({
                .name = p.name,
                .where = {.kind = target_kind::parameter, .index = i32(p.field)},
                .type = p.type,
            });
    }
    notes[index].are_defaults_sound = error_count() == errors_before;
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
            // A conversion the program declares is a call like any other, inlined where it stands.
            if (!is_valid(out.at(candidate).intrinsic))
                note_program_call(scope, candidate, where);
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
        [&](ast::member const& m) { return check_member(scope, expr, m); },
        [&](ast::call const& c) { return check_call(scope, expr, c); },
        [&](ast::self_ref const&) { return not_yet("self"); }, [&](ast::void_ref const&) { return void_type; },
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

    if (auto const* const local = scope.find_local(text))
    {
        set_target(file, id, local->where);
        if (local->is_captured)
        {
            report_capture(scope, where, *local);
            return error_type;
        }
        return local->type;
    }

    auto const* const found = names_seen_from(file).get_ptr(text);
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
    case symbol_kind::pipeline:
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               cc::format("{} is a pipeline, which the host acquires and no shader reads", text));
        break;
    case symbol_kind::constant:
        // CHK-219: a const is its value, and one that did not check is silent here, as a failed symbol always is.
        if (demand(symbol, file, where) == symbol_state::checked)
            return out.at(symbol).type;
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
    if (object_name != nullptr && scope.find_local(text_of(file, object_name->where)) == nullptr)
    {
        auto const* const found = names_seen_from(file).get_ptr(text_of(file, object_name->where));
        if (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::binding)
        {
            auto const binding = found->front();
            auto const object_where = span_of(file, member.object);
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = binding});
            if (demand(binding, file, object_where) != symbol_state::checked)
                return error_type;
            // A const's value is checked outside every function, and a binding member is read only inside one.
            if (!is_valid(scope.function))
            {
                unsupported(file, span_of(file, id), "a binding member outside a function");
                return error_type;
            }

            auto is_listed = false;
            for (auto const listed : out.at(out.functions[out.at(scope.function).info].bindings))
                is_listed = is_listed || listed == binding;
            // CHK-228: a test lists no binding, and one its function lists is a value of the function's run
            auto is_captured = false;
            if (scope.is_test && is_valid(scope.enclosing))
                for (auto const listed : out.at(out.functions[out.at(scope.enclosing).info].bindings))
                    is_captured = is_captured || listed == binding;
            if (is_captured)
                report(diagnostic_kind::test_captures_runtime_value, file, object_where,
                       cc::format("{} is a binding of {}, and a test runs on its own", out.at(binding).name,
                                  out.at(scope.enclosing).name));
            else if (!is_listed && scope.is_test)
                report(diagnostic_kind::binding_not_listed, file, object_where,
                       cc::format("{} is a binding, and a test lists none", out.at(binding).name));
            else if (!is_listed)
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
            auto is_handed = false;
            for (auto const h : handed)
                is_handed = is_handed || h == id;
            if (type != error_type && is_resource(out.at(type).kind) && out.at(type).kind != type_kind::buffer
                && !is_handed)
            {
                unsupported(
                    file, span_of(file, id),
                    cc::format("{} as a value; hand it to a builtin, as in `DEBUG_load(t, xy, 0)`", out.name_of(type)));
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

        handed.push_back(a.value);
        auto const type = check_expr(scope, a.value);
        handed.pop_back();
        if (type == error_type)
        {
            result.is_poisoned = true;
            continue;
        }
        if (!a.is_splat)
        {
            result.written.push_back({.expr = a.value});
            result.types.push_back(type);
            result.names.push_back(text_of(file, a.name));
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
        {
            auto member = i32(0);
            for (auto const& m : out.at(out.at(type).members))
            {
                result.written.push_back({.expr = a.value, .splat_member = member++});
                result.types.push_back(m.type);
                result.names.push_back({});
                result.is_poisoned = result.is_poisoned || m.type == error_type;
            }
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

cc::string checker::call_text(cc::string_view spelling, call_arguments const& arguments) const
{
    auto text = cc::string(spelling);
    text += "(";
    for (auto i = isize(0); i < arguments.types.size(); ++i)
    {
        if (i > 0)
            text += ", ";
        if (!arguments.names[i].empty())
            text.appendf("{} = ", arguments.names[i]);
        text += out.name_of(arguments.types[i]);
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
            && arguments.types[0] == arguments.types[1] && out.is_plain_enum(arguments.types[0]))
        {
            auto const as_int = type_of_builtin(builtins::k_int, file, where);
            arguments.types[0] = as_int;
            arguments.types[1] = as_int;
        }
        // `==` and `!=` over void are the language's own as well: void has one value, so the answer is known.
        if ((spelling == "==" || spelling == "!=") && arguments.types.size() == 2 && arguments.types[0] == void_type
            && arguments.types[1] == void_type)
            return type_of_builtin(builtins::k_bool, file, where);
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
    if (auto const* const local = scope.find_local(text))
    {
        set_target(file, call.callee, local->where);
        (void)check_arguments(scope, call.arguments, false);
        if (local->is_captured)
            report_capture(scope, callee_where, *local);
        else
            unsupported(file, callee_where, "a call of a local value");
        return error_type;
    }

    auto const* const found = names_seen_from(file).get_ptr(text);
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
    {
        // CHK-75: a call of a struct's name is a call of its overload set, its synthesized constructor among it.
        auto const arguments = check_arguments(scope, call.arguments, true);
        if (demand(first, file, callee_where) != symbol_state::checked)
            return error_type;
        auto const type = out.at(first).type;
        auto functions = cc::vector<symbol_id>();
        for (auto const candidate : *found)
            if (out.at(candidate).kind == symbol_kind::function)
                functions.push_back(candidate);
        if (functions.empty())
        {
            set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{} is opaque and has no constructor", out.name_of(type)));
            return type;
        }
        auto const result = resolve_overload(scope, id, call.callee, functions, arguments, text);
        // A call that names a struct is of that struct, so one bad argument does not take the whole value with it.
        return result == error_type ? type : result;
    }
    case symbol_kind::function:
    {
        auto const result
            = resolve_overload(scope, id, call.callee, *found, check_arguments(scope, call.arguments, false), text);
        if (result != error_type)
            judge_filtering(file, where, call.arguments);
        return result;
    }
    case symbol_kind::binding:
        (void)check_arguments(scope, call.arguments, false);
        set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
        report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
               cc::format("{} is a binding, and a call needs a function or a struct", text));
        return error_type;
    case symbol_kind::enumeration:
    case symbol_kind::pipeline:
    case symbol_kind::constant:
        (void)check_arguments(scope, call.arguments, false);
        set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
        report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
               cc::format("{} is {}, and a call needs a function or a struct", text,
                          out.at(first).kind == symbol_kind::pipeline   ? "a pipeline"
                          : out.at(first).kind == symbol_kind::constant ? "a const"
                                                                        : "an enum"));
        return error_type;
    case symbol_kind::unsupported:
        (void)check_arguments(scope, call.arguments, false);
        return error_type;
    }
    return error_type;
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
    auto match_slots = cc::vector<cc::vector<i32>>();
    for (auto const candidate : candidates)
    {
        if (is_out_of_the_running(candidate, arguments))
            continue;
        if (demand(candidate, file, where) != symbol_state::checked)
        {
            is_silent = true;
            continue;
        }
        if (arguments.is_poisoned)
            continue;
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        auto bound = bind_arguments(parameters, arguments);
        if (bound.failure != bind_failure::none || !converts(parameters, arguments, bound.slots))
            continue;
        matches.push_back(candidate);
        match_slots.push_back(cc::move(bound.slots));
    }

    if (matches.empty())
    {
        if (is_silent)
            return error_type;
        // A struct's one constructor says what it takes, which is what a reader needs to fix the call.
        auto const is_constructor = candidates.size() == 1 && out.at(candidates[0]).role == function_role::constructor;
        if (is_constructor)
        {
            auto expected = cc::vector<type_id>();
            for (auto const& p : out.at(out.functions[out.at(candidates[0]).info].parameters))
                expected.push_back(p.type);
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{}, and the constructor is {}", call_text(spelling, arguments),
                              signature_text(spelling, expected)));
        }
        else
            report(diagnostic_kind::no_matching_overload, file, where, call_text(spelling, arguments));
        return error_type;
    }
    // CHK-192: a match of the program's file hides every match of the prelude
    auto is_program_match = false;
    for (auto const m : matches)
        is_program_match = is_program_match || !is_prelude_file(out.at(m).file);
    if (is_program_match)
        for (auto i = matches.size() - 1; i >= 0; --i)
            if (is_prelude_file(out.at(matches[i]).file))
            {
                matches.remove_at(i);
                match_slots.remove_at(i);
            }
    if (matches.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where,
               cc::format("{} has {} candidates", call_text(spelling, arguments), matches.size()));
        return error_type;
    }

    auto const chosen = matches.front();
    auto const& chosen_symbol = out.at(chosen);
    // A synthesized constructor is its struct's, which is what an editor goes to.
    auto const self = chosen_symbol.role == function_role::constructor
                        ? target{.kind = target_kind::constructor, .symbol = chosen_symbol.owner}
                        : target{.kind = target_kind::overload, .symbol = chosen};
    set_target(file, id, self);
    set_target(file, callee, self);
    record_call(file, id, chosen, arguments, match_slots.front());
    auto const result = out.functions[chosen_symbol.info].result;
    if (chosen_symbol.role == function_role::constructor)
        set_type(file, callee, result);
    if (is_valid(chosen_symbol.intrinsic) || chosen_symbol.role == function_role::constructor)
        return result;

    note_program_call(scope, chosen, where);
    return result;
}

bound_arguments checker::bind_arguments(cc::span<parameter const> parameters, call_arguments const& arguments) const
{
    auto result = bound_arguments{.slots = cc::vector<i32>::create_filled(parameters.size(), -1)};
    auto const fail = [&](bind_failure why, i32 argument, i32 parameter)
    {
        result.failure = why;
        result.argument = argument;
        result.parameter = parameter;
        return result;
    };

    // CHK-251: a positional argument after a named one binds only where every argument before it is in its own slot.
    auto is_in_own_slot = true;
    auto seen_named = false;
    for (auto i = i32(0); i < i32(arguments.written.size()); ++i)
    {
        auto const name = arguments.names[i];
        auto p = i32(-1);
        if (name.empty())
        {
            if (seen_named && !is_in_own_slot)
                return fail(bind_failure::positional_out_of_slot, i, -1);
            if (i >= i32(parameters.size()))
                return fail(bind_failure::too_many, i, -1);
            if (parameters[i].is_named_only)
                return fail(bind_failure::positional_to_named_only, i, i);
            p = i;
        }
        else
        {
            seen_named = true;
            // the receiver is filled by its position, never by the name `self`
            for (auto k = i32(0); k < i32(parameters.size()); ++k)
                if (parameters[k].name == name && name != "self")
                    p = k;
            if (p < 0)
                return fail(bind_failure::no_such_parameter, i, -1);
        }
        is_in_own_slot = is_in_own_slot && p == i;
        if (result.slots[p] >= 0)
            return fail(bind_failure::filled_twice, i, p);
        result.slots[p] = i;
    }
    for (auto p = i32(0); p < i32(parameters.size()); ++p)
        if (result.slots[p] < 0 && !parameters[p].has_default)
            return fail(bind_failure::missing_argument, -1, p);
    return result;
}

bool checker::converts(cc::span<parameter const> parameters, call_arguments const& arguments, cc::span<i32 const> slots) const
{
    for (auto p = isize(0); p < parameters.size(); ++p)
        if (slots[p] >= 0 && !takes(parameters[p].type, arguments.types[slots[p]]))
            return false;
    return true;
}

void checker::record_call(i32 file,
                          ast::expr_id id,
                          symbol_id callee,
                          call_arguments const& arguments,
                          cc::span<i32 const> slots)
{
    auto const written = ast::range_of<written_argument>{.first = u32(out.written_arguments.size()),
                                                         .count = u32(arguments.written.size())};
    out.written_arguments.push_back_range(arguments.written);
    auto const slot_range = ast::range_of<i32>{.first = u32(out.call_slots.size()), .count = u32(slots.size())};
    out.call_slots.push_back_range(slots);
    out.files[file].call_of[ast::index_of(id)] = i32(out.call_records.size());
    out.call_records.push_back({.callee = callee, .written = written, .slots = slot_range});
}

void checker::note_program_call(function_scope const& scope, symbol_id callee, source_span where)
{
    auto const file = scope.file;
    // A call of a function of the program is inlined, so it is an edge recursion is looked for along.
    calls.push_back({.caller = scope.function, .callee = callee, .file = file, .where = where});

    // Bindings are an effect: what the callee reads, the caller has to list, and so on up to the entry point.
    auto const listed = out.at(out.functions[out.at(scope.function).info].bindings);
    for (auto const needed : out.at(out.functions[out.at(callee).info].bindings))
    {
        auto is_listed = false;
        for (auto const l : listed)
            is_listed = is_listed || l == needed;
        if (!is_listed && scope.is_test)
        {
            // CHK-228: a test gives a callee its bindings through a local binding, which delegation will carry
            auto& d = report(
                diagnostic_kind::binding_not_listed, file, where,
                cc::format("{} needs {}, and a test lists no binding", out.at(callee).name, out.at(needed).name));
            d.notes.push_back({.file = file,
                               .where = where,
                               .message = cc::format("a `binding {}:` declared in the test gives {} its values, "
                                                     "once local bindings are carried",
                                                     out.at(needed).name, out.at(callee).name)});
        }
        else if (!is_listed)
            report(diagnostic_kind::binding_not_listed, file, where,
                   cc::format("{} needs {}, which is not in the binding list of {}", out.at(callee).name,
                              out.at(needed).name, out.at(scope.function).name));
    }
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

bool checker::is_out_of_the_running(symbol_id candidate, call_arguments const& arguments) const
{
    auto const& s = out.at(candidate);
    if (s.kind != symbol_kind::function || s.state != symbol_state::in_compilation || s.info < 0)
        return false;
    auto const parameters = out.at(out.functions[s.info].parameters);
    auto const bound = bind_arguments(parameters, arguments);
    return bound.failure != bind_failure::none || !converts(parameters, arguments, bound.slots);
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
    auto positional = call_arguments{.types = cc::vector<type_id>::create_copy_of(types)};
    for (auto i = isize(0); i < types.size(); ++i)
    {
        positional.written.push_back({});
        positional.names.push_back({});
    }
    if (auto const* const found = operators.get_ptr(spelling))
        for (auto const candidate : *found)
        {
            if (is_out_of_the_running(candidate, positional))
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
