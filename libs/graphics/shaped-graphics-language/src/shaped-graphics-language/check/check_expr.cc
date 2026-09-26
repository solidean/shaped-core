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

/// Appends one argument to every parallel list of `a`.
void add_argument(call_arguments& a,
                  written_argument w,
                  type_id type,
                  cc::string_view name = {},
                  number_literal number = {},
                  i32 literal = -1)
{
    a.written.push_back(w);
    a.types.push_back(type);
    a.names.push_back(name);
    a.numbers.push_back(number);
    a.literals.push_back(literal);
}
} // namespace

// ---- bodies ---------------------------------------------------------------------------------------------------------

void checker::check_body(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const index = out.at(id).info;
    if (notes[index].is_body_checked)
        return;
    auto const& node = ast.at(out.at(id).declaration).node;
    auto const* const f = node.try_as<ast::fun_decl>();
    auto const* const property = node.try_as<ast::property_decl>();
    if (f == nullptr && property == nullptr)
        return;
    auto const& body = f != nullptr ? f->body : property->body;
    auto const name = f != nullptr ? f->name : property->name;
    if (body.kind == ast::body_kind::none)
        return;
    notes[index].is_body_checked = true;

    // by value: checking the body may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    auto scope = function_scope{.function = id, .file = file, .result = info.result};
    // CHK-245: `self` is the receiver of a method and of a property, whose members a bare name also finds (CHK-62)
    auto const role = out.at(id).role;
    auto const has_receiver
        = role == function_role::property || (role == function_role::method && f->receiver == ast::receiver_kind::self);
    auto const parameters = out.at(info.parameters);
    for (auto i = isize(0); i < parameters.size(); ++i)
    {
        auto const& p = parameters[i];
        if (has_receiver && i == 0)
        {
            scope.receiver = p.type;
            scope.locals.push_back({.name = "self", .where = {.kind = target_kind::receiver}, .type = p.type});
            continue;
        }
        // CHK-54: a parameter may have the name of a module-level symbol, which it hides in the body.
        judge_shadowing(file, text_of(file, ast.at(p.field).name), ast.at(p.field).name);
        scope.locals.push_back({
            .name = text_of(file, ast.at(p.field).name),
            .where = {.kind = target_kind::parameter, .index = i32(p.field)},
            .type = p.type,
        });
    }

    auto const errors_before = error_count();

    // A property's `=>:` block is a value block: it has a value only through `yield` (AST-81).
    if (property != nullptr && !ast::is_valid(body.value))
    {
        scope.value_blocks.push_back({});
        (void)check_statements(scope, body.statements);
        auto const yielded = scope.value_blocks.back();
        scope.value_blocks.remove_back();
        auto const value = yielded.has_yield ? yielded.value : error_type;
        if (notes[index].infers_result)
            out.functions[index].result = value;
        else if (value != error_type && info.result != error_type && value != info.result)
            report(diagnostic_kind::type_mismatch, file, name,
                   cc::format("{} is {}, and its block yields {}", out.at(id).name, out.name_of(info.result),
                              out.name_of(value)));
        notes[index].is_body_sound = error_count() == errors_before;
        return;
    }

    auto ending = check_statements(scope, body.statements);
    if (ast::is_valid(body.value) && notes[index].infers_result)
    {
        out.functions[index].result = check_expr(scope, body.value);
        ending = flow::exits;
    }
    else if (ast::is_valid(body.value))
    {
        check_return(scope, span_of(file, body.value), body.value);
        ending = flow::exits;
    }
    // a block without a statement was reported where it was parsed
    auto const has_statements = !body.statements.empty() || ast::is_valid(body.value);
    if (has_statements && ending == flow::falls_through && info.result != void_type && info.result != error_type)
        report(diagnostic_kind::missing_return, file, name,
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
            (void)check_expected(scope, ast.at(p.field).default_value, p.type, cc::format("the default of {}", p.name));
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
        [&](ast::self_ref const&)
        {
            // CHK-245: `self` is the receiver of a method or a property, and names nothing anywhere else
            auto const* const local = is_valid(scope.receiver) ? scope.find_local("self") : nullptr;
            if (local == nullptr)
            {
                report(diagnostic_kind::unknown_name, file, where, "self");
                return error_type;
            }
            set_target(file, expr, local->where);
            if (local->is_captured)
            {
                report_capture(scope, where, *local);
                return error_type;
            }
            return local->type;
        },
        [&](ast::void_ref const&) { return void_type; },
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

    // CHK-62: in a method or a property, a field or a property of `self` comes before the module
    if (is_valid(scope.receiver) && scope.receiver != error_type)
    {
        auto const& receiver = out.at(scope.receiver);
        auto const fields = out.at(receiver.members);
        for (auto i = isize(0); i < fields.size(); ++i)
            if (fields[i].name == text)
            {
                set_target(file, id, {.kind = target_kind::field, .symbol = receiver.symbol, .index = i32(i)});
                return fields[i].type;
            }
        auto properties = cc::vector<symbol_id>();
        for (auto const candidate : candidates_of(file, text, scope.receiver))
            if (out.at(candidate).role == function_role::property)
                properties.push_back(candidate);
        if (!properties.empty())
        {
            auto arguments = call_arguments();
            add_argument(arguments, {.is_receiver = true}, scope.receiver);
            return resolve_overload(scope, id, ast::expr_id::none, properties, arguments, text, call_spelling::dot_read);
        }
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
                unsupported(file, span_of(file, id),
                            cc::format("{} as a value; call a builtin on it, as in `t.load(xy)`", out.name_of(type)));
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

    // CHK-249: `a.foo` is the field where there is one, and a call of `foo` with `a` otherwise
    auto const& type = out.at(object);
    auto const index = find_member(type.members);
    if (index >= 0)
    {
        set_target(file, id, {.kind = target_kind::field, .symbol = type.symbol, .index = i32(index)});
        return out.at(type.members)[index].type;
    }
    auto const candidates = candidates_of(file, name, object);
    if (candidates.empty())
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}", out.name_of(object), name));
        return error_type;
    }
    auto arguments = call_arguments();
    add_argument(arguments, {.expr = member.object}, object);
    return resolve_overload(scope, id, ast::expr_id::none, candidates, arguments, name, call_spelling::dot_read);
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

        // CHK-81: a tuple or an object literal converts to the parameter it meets, so it has no type of its own yet
        auto const* const value = ast::is_valid(a.value) ? &ast_of(file).at(a.value).node : nullptr;
        if (!a.is_splat && value != nullptr && (value->is<ast::tuple>() || value->is<ast::object>()))
        {
            auto const literal = shape_literal(scope, a.value);
            result.is_poisoned = result.is_poisoned || literals[literal].is_poisoned;
            add_argument(result, {.expr = a.value}, type_id::none, text_of(file, a.name), {}, literal);
            continue;
        }

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
            add_argument(result, {.expr = a.value}, type, text_of(file, a.name), number_of(file, a.value));
            continue;
        }

        if (!is_constructor)
        {
            unsupported(file, where, "a splat outside a constructor call");
            result.is_poisoned = true;
        }
        else if (out.at(type).is_opaque || out.at(out.at(type).members).empty())
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
                add_argument(result, {.expr = a.value, .splat_member = member++}, m.type);
                result.is_poisoned = result.is_poisoned || m.type == error_type;
            }
        }
    }
    return result;
}

number_literal checker::number_of(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr) || !ast_of(file).at(expr).node.is<ast::literal>())
        return {};
    auto const text = text_of(file, span_of(file, expr));
    switch (classify_number(text))
    {
    case number_class::plain_integer:
    {
        auto const value = parse_plain_integer(text);
        if (!value.has_value())
            return {};
        return {.is_number = true, .is_integer = true, .integer = value.value()};
    }
    case number_class::plain_float:
    {
        auto const value = parse_plain_float(text);
        if (!value.has_value())
            return {};
        return {.is_number = true, .real = value.value()};
    }
    case number_class::other:
        break;
    }
    return {};
}

i32 checker::shape_literal(function_scope& scope, ast::expr_id expr)
{
    auto const file = scope.file;
    auto const& node = ast_of(file).at(expr).node;
    auto const* const tuple = node.try_as<ast::tuple>();
    auto const elements = ast_of(file).at(tuple != nullptr ? tuple->elements : node.as<ast::object>().elements);
    auto shaped = call_arguments();
    for (auto const& e : elements)
    {
        auto const where = span_of(file, e.form);
        judge_attributes(file, e.attributes, {}, "an element");
        // CHK-86
        if (e.is_splat || e.is_shorthand)
        {
            unsupported(file, where, e.is_splat ? "a splat in a literal" : "a shorthand object element");
            shaped.is_poisoned = true;
            continue;
        }
        auto const* const inner = ast::is_valid(e.value) ? &ast_of(file).at(e.value).node : nullptr;
        if (inner != nullptr && (inner->is<ast::tuple>() || inner->is<ast::object>()))
        {
            auto const literal = shape_literal(scope, e.value);
            shaped.is_poisoned = shaped.is_poisoned || literals[literal].is_poisoned;
            add_argument(shaped, {.expr = e.value}, type_id::none, text_of(file, e.name), {}, literal);
            continue;
        }
        auto const type = check_expr(scope, e.value);
        if (type == error_type)
        {
            shaped.is_poisoned = true;
            continue;
        }
        add_argument(shaped, {.expr = e.value}, type, text_of(file, e.name), number_of(file, e.value));
    }
    literals.push_back(cc::move(shaped));
    return i32(literals.size() - 1);
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
        // a tuple or an object literal has no type until the parameter it meets gives it one
        if (arguments.literals[i] >= 0)
            text += "a literal";
        else
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
    if (callee.node.is<ast::member>())
        return check_dot_call(scope, id, call);
    if (n == nullptr)
    {
        (void)check_arguments(scope, call.arguments, false);
        if (callee.node.is<ast::index>())
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
    if (found == nullptr || found->empty() || out.at(found->front()).kind == symbol_kind::function)
    {
        // CHK-247: the functions of its name, and those of its first argument's type scope
        auto const arguments = check_arguments(scope, call.arguments, false);
        auto const first
            = arguments.written.empty() || arguments.written[0].splat_member > 0 ? type_id::none : arguments.types[0];
        auto const candidates = candidates_of(file, text, first);
        if (candidates.empty())
        {
            report(diagnostic_kind::unknown_name, file, callee_where, text);
            return error_type;
        }
        auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, text);
        if (result != error_type)
            judge_filtering(file, where, arguments.written);
        return result;
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
        return error_type;
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

type_id checker::check_dot_call(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& member = ast.at(call.callee).node.as<ast::member>();
    auto const name = text_of(file, member.name);

    // `T.foo(…)`: the functions of the type scope of `T`, and `T` is no argument (CHK-248)
    auto const* const object_name
        = ast::is_valid(member.object) ? ast.at(member.object).node.try_as<ast::name>() : nullptr;
    if (object_name != nullptr && scope.find_local(text_of(file, object_name->where)) == nullptr)
    {
        auto const* const found = names_seen_from(file).get_ptr(text_of(file, object_name->where));
        auto const kind = found != nullptr && !found->empty() ? out.at(found->front()).kind : symbol_kind::unsupported;
        if (kind == symbol_kind::structure || kind == symbol_kind::enumeration)
        {
            auto const owner = found->front();
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = owner});
            auto const arguments = check_arguments(scope, call.arguments, false);
            if (demand(owner, file, span_of(file, member.object)) != symbol_state::checked || member.name.empty())
                return error_type;
            auto candidates = cc::vector<symbol_id>();
            if (auto const* const scope_of = type_scopes.get_ptr(i32(index_of(owner))))
                if (auto const* const functions = scope_of->get_ptr(name))
                    for (auto const f : *functions)
                        if (is_visible_from(file, f))
                            candidates.push_back(f);
            if (candidates.empty())
            {
                report(diagnostic_kind::unknown_member, file, member.name,
                       cc::format("{} has no function {}", out.at(owner).name, name));
                return error_type;
            }
            return resolve_overload(scope, id, call.callee, candidates, arguments,
                                    cc::format("{}.{}", out.at(owner).name, name));
        }
        if (kind == symbol_kind::binding)
        {
            (void)check_arguments(scope, call.arguments, false);
            unsupported(file, span_of(file, call.callee), "a call through a binding");
            return error_type;
        }
    }

    // `a.foo(…)` is `foo(a, …)`, and a field of `a` takes part in no call (CHK-249)
    handed.push_back(member.object);
    auto const receiver = check_expr(scope, member.object);
    handed.pop_back();
    auto arguments = check_arguments(scope, call.arguments, false);
    if (receiver == error_type || member.name.empty())
        return error_type;
    arguments.written.insert_at(0, {.expr = member.object});
    arguments.types.insert_at(0, receiver);
    arguments.names.insert_at(0, {});
    arguments.numbers.insert_at(0, {});
    arguments.literals.insert_at(0, -1);

    auto const candidates = candidates_of(file, name, receiver);
    if (candidates.empty())
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}", out.name_of(receiver), name));
        return error_type;
    }
    auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, name, call_spelling::dot_call);
    if (result != error_type)
        judge_filtering(file, span_of(file, id), arguments.written);
    return result;
}

type_id checker::resolve_overload(function_scope& scope,
                                  ast::expr_id id,
                                  ast::expr_id callee,
                                  cc::span<symbol_id const> candidates,
                                  call_arguments const& arguments,
                                  cc::string_view spelling,
                                  call_spelling written_as)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);

    auto is_silent = arguments.is_poisoned;
    auto matches = cc::vector<candidate_match>();
    for (auto const candidate : candidates)
    {
        if (is_out_of_the_running(file, candidate, arguments))
            continue;
        if (demand(candidate, file, where) != symbol_state::checked)
        {
            is_silent = true;
            continue;
        }
        if (arguments.is_poisoned)
            continue;
        if (auto m = match(file, candidate, arguments); m.has_value())
            matches.push_back(cc::move(m.value()));
    }

    if (matches.empty())
    {
        if (is_silent)
            return error_type;
        note_near_misses(file, id, candidates, arguments);
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
    auto const best = best_of(cc::move(matches));
    if (best.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where,
               cc::format("{} has {} candidates", call_text(spelling, arguments), best.size()));
        return error_type;
    }

    auto const& chosen_match = best.front();
    auto const chosen = chosen_match.candidate;
    auto const& chosen_symbol = out.at(chosen);
    // CHK-256: the spelling says what the writer means, and is checked on the target it chose
    if (written_as == call_spelling::dot_read && chosen_symbol.role != function_role::property)
        report(diagnostic_kind::call_spelling, file, where,
               cc::format("{} is a function, so it is called: write .{}()", spelling, spelling));
    else if (written_as == call_spelling::dot_call && chosen_symbol.role == function_role::property)
        report(diagnostic_kind::call_spelling, file, where,
               cc::format("{} is a property, so it is read: write .{}", spelling, spelling));

    // A synthesized constructor is its struct's, which is what an editor goes to.
    auto const self = chosen_symbol.role == function_role::constructor
                        ? target{.kind = target_kind::constructor, .symbol = chosen_symbol.owner}
                        : target{.kind = target_kind::overload, .symbol = chosen};
    set_target(file, id, self);
    set_target(file, callee, self);
    record_call(file, id, chosen, arguments, chosen_match.slots);
    // by value: resolving a literal argument may compile another function, and `functions` then moves
    auto const info = out.functions[chosen_symbol.info];
    commit_literals(scope, arguments, out.at(info.parameters), chosen_match.slots);
    if (chosen_symbol.role == function_role::constructor)
        set_type(file, callee, info.result);
    if (is_valid(out.at(chosen).intrinsic))
        return info.result;

    // A construction is written where it stands, and the defaults of its fields with it, so what they call is reached.
    note_program_call(scope, chosen, where);
    return info.result;
}

void checker::note_near_misses(i32 file,
                               ast::expr_id call,
                               cc::span<symbol_id const> candidates,
                               call_arguments const& arguments)
{
    for (auto const candidate : candidates)
    {
        if (out.at(candidate).state != symbol_state::checked)
            continue;
        auto const parameters
            = cc::vector<parameter>::create_copy_of(out.at(out.functions[out.at(candidate).info].parameters));
        auto const bound = bind_arguments(parameters, arguments);
        auto miss = near_miss{.file = file,
                              .call = call,
                              .candidate = candidate,
                              .reason = bound.failure,
                              .argument = bound.argument,
                              .parameter = bound.parameter};
        for (auto p = isize(0); miss.reason == miss_reason::none && p < parameters.size(); ++p)
        {
            auto at_default = 0;
            auto const i = bound.slots[p];
            if (i >= 0 && !chain_of(file, parameters[p].type, arguments, i, at_default).has_value())
                miss = {.file = file,
                        .call = call,
                        .candidate = candidate,
                        .reason = miss_reason::no_conversion,
                        .argument = i,
                        .parameter = i32(p)};
        }
        out.near_misses.push_back(miss);
    }
}

cc::optional<candidate_match> checker::match(i32 file, symbol_id candidate, call_arguments const& arguments)
{
    // by value: a literal argument may compile another function, and `parameters` then moves
    auto const range = out.functions[out.at(candidate).info].parameters;
    auto const parameters = cc::vector<parameter>::create_copy_of(out.at(range));
    auto bound = bind_arguments(parameters, arguments);
    if (bound.failure != miss_reason::none)
        return cc::nullopt;
    auto result = candidate_match{.candidate = candidate,
                                  .slots = cc::move(bound.slots),
                                  .chains = cc::vector<i32>::create_filled(arguments.written.size(), 0)};
    for (auto p = isize(0); p < parameters.size(); ++p)
    {
        auto const i = result.slots[p];
        if (i < 0)
            continue;
        auto const length = chain_of(file, parameters[p].type, arguments, i, result.at_default);
        if (!length.has_value())
            return cc::nullopt;
        result.chains[i] = length.value();
    }
    return result;
}

cc::optional<i32> checker::chain_of(i32 file, type_id parameter, call_arguments const& arguments, isize i, i32& at_default)
{
    if (arguments.literals[i] >= 0)
        return literal_chain(file, parameter, arguments.literals[i], at_default);
    if (arguments.numbers[i].is_number)
    {
        // CHK-253: a number literal converts to what holds it, at no cost; its default type is what it was checked as
        if (!holds(arguments.numbers[i], parameter))
            return cc::nullopt;
        at_default += parameter == arguments.types[i] ? 1 : 0;
        return 0;
    }
    return takes(parameter, arguments.types[i]) ? cc::optional<i32>(0) : cc::nullopt;
}

cc::vector<symbol_id> checker::functions_named_after(i32 file, type_id to) const
{
    auto result = cc::vector<symbol_id>();
    if (!is_valid(to) || to == error_type || out.at(to).kind != type_kind::structure)
        return result;
    if (auto const* const found = names_seen_from(file).get_ptr(out.at(out.at(to).symbol).name))
        for (auto const id : *found)
            if (out.at(id).kind == symbol_kind::function)
                result.push_back(id);
    return result;
}

cc::optional<i32> checker::literal_chain(i32 file, type_id to, i32 literal, i32& at_default)
{
    auto matches = cc::vector<candidate_match>();
    for (auto const candidate : functions_named_after(file, to))
    {
        if (out.at(candidate).state == symbol_state::in_compilation)
            continue;
        if (demand(candidate, file, {}) != symbol_state::checked)
            continue;
        // by value: matching may compile a body that shapes literals of its own, and `literals` then moves
        auto const arguments = literals[literal];
        if (auto m = match(file, candidate, arguments); m.has_value())
            matches.push_back(cc::move(m.value()));
    }
    auto const best = best_of(cc::move(matches));
    if (best.size() != 1)
        return cc::nullopt;
    at_default += best.front().at_default;
    auto longest = 0;
    for (auto const c : best.front().chains)
        longest = c > longest ? c : longest;
    return longest + 1;
}

cc::vector<candidate_match> checker::best_of(cc::vector<candidate_match> matches) const
{
    // CHK-192: a match of the program's file hides every match of the prelude
    auto is_program_match = false;
    for (auto const& m : matches)
        is_program_match = is_program_match || !is_prelude_file(out.at(m.candidate).file);
    if (is_program_match)
        matches.remove_all_where([&](candidate_match const& m) { return is_prelude_file(out.at(m.candidate).file); });

    // CHK-254: a candidate no worse at any argument and better at one is better; the best is better than every other
    auto const is_better = [](candidate_match const& a, candidate_match const& b)
    {
        auto is_shorter = false;
        for (auto i = isize(0); i < a.chains.size(); ++i)
        {
            if (a.chains[i] > b.chains[i])
                return false;
            is_shorter = is_shorter || a.chains[i] < b.chains[i];
        }
        return is_shorter;
    };
    auto result = cc::vector<candidate_match>();
    for (auto const& m : matches)
    {
        auto is_beaten = false;
        for (auto const& other : matches)
            is_beaten = is_beaten || is_better(other, m);
        if (!is_beaten)
            result.push_back(m);
    }
    if (result.size() < 2)
        return result;

    // CHK-255: only between candidates whose chains agree at every argument
    for (auto const& m : result)
        for (auto i = isize(0); i < m.chains.size(); ++i)
            if (m.chains[i] != result.front().chains[i])
                return result;
    auto most = 0;
    for (auto const& m : result)
        most = m.at_default > most ? m.at_default : most;
    result.remove_all_where([&](candidate_match const& m) { return m.at_default < most; });
    auto const is_of_type = [&](candidate_match const& m)
    { return is_valid(out.at(m.candidate).owner) && out.at(m.candidate).role != function_role::constructor; };
    auto type_scoped = isize(0);
    for (auto const& m : result)
        type_scoped += is_of_type(m) ? 1 : 0;
    if (type_scoped > 0 && type_scoped < result.size())
        result.remove_all_where([&](candidate_match const& m) { return !is_of_type(m); });
    return result;
}

type_id checker::prelude_type(cc::string_view name) const
{
    auto const* const found = prelude_names.get_ptr(name);
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::structure)
        return type_id::none;
    return out.at(found->front()).type;
}

bool checker::holds(number_literal const& n, type_id to) const
{
    if (!is_valid(to))
        return false;
    if (to == prelude_type(builtins::k_int))
        return n.is_integer && n.integer >= -2147483648ll && n.integer <= 2147483647ll;
    if (to == prelude_type(builtins::k_uint))
        return n.is_integer && n.integer >= 0 && n.integer <= 4294967295ll;
    if (to == prelude_type(builtins::k_float))
    {
        // an integer converts only where the float holds it exactly, and a float rounds to nearest within range
        if (n.is_integer)
            return f64(f32(n.integer)) == f64(n.integer);
        auto const magnitude = n.real < 0 ? -n.real : n.real;
        return magnitude <= 3.4028234663852886e38;
    }
    return false;
}

void checker::commit_literals(function_scope& scope,
                              call_arguments const& arguments,
                              cc::span<parameter const> parameters,
                              cc::span<i32 const> slots)
{
    // by value: resolving a literal may compile another function, and `parameters` then moves
    auto const copied = cc::vector<parameter>::create_copy_of(parameters);
    for (auto p = isize(0); p < copied.size(); ++p)
    {
        auto const i = slots[p];
        if (i < 0)
            continue;
        if (arguments.numbers[i].is_number)
            set_type(scope.file, arguments.written[i].expr, copied[p].type);
        else if (arguments.literals[i] >= 0)
            (void)resolve_literal(scope, arguments.written[i].expr, copied[p].type, arguments.literals[i]);
    }
}

type_id checker::resolve_literal(function_scope& scope, ast::expr_id expr, type_id to, i32 literal)
{
    auto const file = scope.file;
    auto const where = span_of(file, expr);
    if (to == error_type || literals[literal].is_poisoned)
        return error_type;
    auto const candidates = functions_named_after(file, to);
    if (candidates.empty())
    {
        report(diagnostic_kind::type_mismatch, file, where,
               out.at(to).kind == type_kind::structure
                   ? cc::format("{} is opaque, so no literal converts to it", out.name_of(to))
                   : cc::format("a literal converts to a struct, and {} is none", out.name_of(to)));
        return error_type;
    }
    // by value: `literals` may grow while the call resolves
    auto const arguments = literals[literal];
    auto const result
        = resolve_overload(scope, expr, ast::expr_id::none, candidates, arguments, out.at(out.at(to).symbol).name);
    set_type(file, expr, to);
    // CHK-85: a function of the type's name that gives another type is called by name, and converts nothing
    if (result != error_type && result != to)
        report(diagnostic_kind::literal_conversion_result, file, where,
               cc::format("this literal converts by {}, which returns {} and not {}",
                          out.at(out.files[file].target_at(expr).symbol).name, out.name_of(result), out.name_of(to)));
    return result == error_type ? error_type : to;
}

type_id checker::check_expected(function_scope& scope, ast::expr_id expr, type_id to, cc::string_view what)
{
    auto const file = scope.file;
    if (!ast::is_valid(expr))
        return error_type;
    auto const& node = ast_of(file).at(expr).node;
    // CHK-82: a tuple or an object literal converts to the type expected where it stands
    if (node.is<ast::tuple>() || node.is<ast::object>())
    {
        auto const literal = shape_literal(scope, expr);
        return resolve_literal(scope, expr, to, literal);
    }
    auto const type = check_expr(scope, expr);
    if (type == error_type || to == error_type)
        return type;
    // CHK-253: a number literal where one type is expected converts to it where it holds exactly
    auto const number = number_of(file, expr);
    if (number.is_number && type != to && prelude_type(out.name_of(to)) == to)
    {
        if (!holds(number, to))
        {
            report(diagnostic_kind::literal_not_representable, file, span_of(file, expr),
                   cc::format("{} does not hold {} exactly", out.name_of(to), text_of(file, span_of(file, expr))));
            return error_type;
        }
        set_type(file, expr, to);
        return to;
    }
    if (type != to)
        report(diagnostic_kind::type_mismatch, file, span_of(file, expr),
               what.empty() ? cc::format("expected {}, got {}", out.name_of(to), out.name_of(type))
                            : cc::format("{} is {}, got {}", what, out.name_of(to), out.name_of(type)));
    return type;
}

bound_arguments checker::bind_arguments(cc::span<parameter const> parameters, call_arguments const& arguments) const
{
    auto result = bound_arguments{.slots = cc::vector<i32>::create_filled(parameters.size(), -1)};
    auto const fail = [&](miss_reason why, i32 argument, i32 parameter)
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
                return fail(miss_reason::positional_out_of_slot, i, -1);
            if (i >= i32(parameters.size()))
                return fail(miss_reason::too_many, i, -1);
            if (parameters[i].is_named_only)
                return fail(miss_reason::positional_to_named_only, i, i);
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
                return fail(miss_reason::no_such_parameter, i, -1);
        }
        is_in_own_slot = is_in_own_slot && p == i;
        if (result.slots[p] >= 0)
            return fail(miss_reason::filled_twice, i, p);
        result.slots[p] = i;
    }
    for (auto p = i32(0); p < i32(parameters.size()); ++p)
        if (result.slots[p] < 0 && !parameters[p].has_default)
            return fail(miss_reason::missing_argument, -1, p);
    return result;
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
        // An operand between two links has the type its first link gave it, so only the new one converts.
        ast::expr_id const both[] = {i == 0 ? operands[0] : ast::expr_id::none, operands[i + 1]};
        auto const chosen = resolve_operator(file, where, spelling, pair, both);
        if (is_valid(chosen) && ast::is_valid(operands[i + 1]))
            types[i + 1] = out.files[file].type_at(operands[i + 1]);
        if (is_valid(chosen) && out.functions[out.at(chosen).info].result != bool_type && bool_type != error_type)
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("a comparison in a chain is a bool, and operator {} gives {}", spelling,
                              out.name_of(out.functions[out.at(chosen).info].result)));
    }
    return bool_type;
}

bool checker::is_out_of_the_running(i32 file, symbol_id candidate, call_arguments const& arguments)
{
    auto const& s = out.at(candidate);
    if (s.kind != symbol_kind::function || s.state != symbol_state::in_compilation || s.info < 0)
        return false;
    return !match(file, candidate, arguments).has_value();
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

symbol_id checker::resolve_operator(i32 file,
                                    source_span where,
                                    cc::string_view spelling,
                                    cc::span<type_id const> types,
                                    cc::span<ast::expr_id const> operands)
{
    auto arguments = call_arguments();
    for (auto i = isize(0); i < types.size(); ++i)
    {
        auto const expr = i < operands.size() ? operands[i] : ast::expr_id::none;
        add_argument(arguments, {.expr = expr}, types[i], {}, number_of(file, expr));
    }

    auto is_silent = false;
    auto matches = cc::vector<candidate_match>();
    if (auto const* const found = operators.get_ptr(spelling))
        for (auto const candidate : *found)
        {
            if (is_out_of_the_running(file, candidate, arguments))
                continue;
            if (demand(candidate, file, where) != symbol_state::checked)
            {
                is_silent = true;
                continue;
            }
            if (auto m = match(file, candidate, arguments); m.has_value())
                matches.push_back(cc::move(m.value()));
        }

    auto const text = signature_text(cc::format("operator {}", spelling), types);
    if (matches.empty())
    {
        if (!is_silent)
            report(diagnostic_kind::no_matching_overload, file, where, text);
        return symbol_id::none;
    }
    auto const best = best_of(cc::move(matches));
    if (best.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where, cc::format("{} has {} candidates", text, best.size()));
        return symbol_id::none;
    }
    auto const chosen = best.front().candidate;
    if (!is_valid(out.at(chosen).intrinsic))
    {
        unsupported(file, where, "an operator of the program's own where no call is written");
        return symbol_id::none;
    }
    // a number literal operand takes the type of the parameter it fills, which is what the flat tree writes
    auto const parameters = out.at(out.functions[out.at(chosen).info].parameters);
    for (auto i = isize(0); i < parameters.size() && i < operands.size(); ++i)
        if (arguments.numbers[i].is_number)
            set_type(file, operands[i], parameters[i].type);
    return chosen;
}
