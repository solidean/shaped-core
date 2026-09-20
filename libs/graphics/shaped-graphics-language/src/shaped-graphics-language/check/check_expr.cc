#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;

local_name const* find_local(function_scope const& scope, cc::string_view name)
{
    for (auto i = scope.locals.size() - 1; i >= 0; --i)
        if (scope.locals[i].name == name)
            return &scope.locals[i];
    return nullptr;
}
} // namespace

// ---- bodies and statements ------------------------------------------------------------------------------------------

void checker::check_body(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const& f = ast.at(out.at(id).declaration).node.as<ast::fun_decl>();
    if (f.body.kind == ast::body_kind::none)
        return;

    auto const info = out.functions[out.at(id).info];
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

    if (ast::is_valid(f.body.value))
        check_return(scope, span_of(file, f.body.value), f.body.value);

    auto const statements = ast.at(f.body.statements);
    for (auto const s : statements)
        check_stmt(scope, s);

    if (!statements.empty())
    {
        auto const* const last = ast.at(statements.back()).node.try_as<ast::expr_stmt>();
        auto const is_return
            = last != nullptr && ast::is_valid(last->value) && ast.at(last->value).node.is<ast::return_expr>();
        if (!is_return && !ast.at(statements.back()).node.is<ast::invalid_stmt>())
            unsupported(file, span_of(file, statements.back()), "a body that does not end in return");
    }

    notes[out.at(id).info].is_body_sound = error_count() == errors_before;
}

void checker::check_stmt(function_scope& scope, ast::stmt_id stmt)
{
    auto const file = scope.file;
    auto const& s = ast_of(file).at(stmt);
    auto const where = span_of(file, stmt);
    judge_attributes(file, s.attributes, {}, "a statement");

    s.node.visit([&](ast::let_stmt const& let) { check_let(scope, stmt, let); },
                 [&](ast::expr_stmt const& e)
                 {
                     auto const* const r
                         = ast::is_valid(e.value) ? ast_of(file).at(e.value).node.try_as<ast::return_expr>() : nullptr;
                     if (r != nullptr)
                         check_return(scope, where, r->value);
                     else if (check_expr(scope, e.value) != error_type)
                         unsupported(file, where, "an expression statement");
                 },
                 [&](ast::assign_stmt const&) { unsupported(file, where, "assignment"); }, [&](ast::if_stmt const&)
                 { unsupported(file, where, "if"); }, [&](ast::for_stmt const&) { unsupported(file, where, "for"); },
                 [&](ast::while_stmt const&) { unsupported(file, where, "while"); },
                 [&](ast::assert_stmt const&) { unsupported(file, where, "assert"); },
                 [&](ast::print_stmt const&) { unsupported(file, where, "print"); }, [&](ast::decl_stmt const&)
                 { unsupported(file, where, "a declaration inside a function"); }, [&](ast::invalid_stmt const&) {});
}

void checker::check_let(function_scope& scope, ast::stmt_id id, ast::let_stmt const& let)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);

    if (let.is_mut)
        unsupported(file, where, "let mut");

    auto type = error_type;
    if (ast::is_valid(let.value))
        type = check_expr(scope, let.value);
    else
        unsupported(file, where, "a let without a value");

    if (ast::is_valid(let.type))
    {
        auto const declared = resolve_type(file, let.type);
        if (type != error_type && declared != error_type && type != declared)
            report(diagnostic_kind::type_mismatch, file, span_of(file, let.value),
                   cc::format("expected {}, got {}", out.name_of(declared), out.name_of(type)));
        type = declared;
    }

    if (!ast::is_valid(let.pattern))
        return;
    auto const* const n = ast.at(let.pattern).node.try_as<ast::name>();
    if (n == nullptr)
    {
        if (!ast.at(let.pattern).node.is<ast::invalid_expr>())
            unsupported(file, span_of(file, let.pattern), "a pattern in let");
        return;
    }

    auto const name = text_of(file, n->where);
    if (find_local(scope, name) != nullptr)
    {
        report(diagnostic_kind::duplicate_declaration, file, n->where, name);
        return;
    }
    if (names.contains(name))
        unsupported(file, n->where, "a local that shadows a module-level name");

    auto const self = target{.kind = target_kind::local, .index = i32(id)};
    set_type(file, let.pattern, type);
    set_target(file, let.pattern, self);
    // only now: the value of `let x = x` does not see the `x` it declares
    scope.locals.push_back({.name = name, .where = self, .type = type});
}

void checker::check_return(function_scope& scope, source_span where, ast::expr_id value)
{
    auto const file = scope.file;
    if (!ast::is_valid(value))
    {
        unsupported(file, where, "a return without a value");
        return;
    }
    if (ast_of(file).at(value).node.is<ast::object>())
    {
        convert_object(scope, value, scope.result);
        return;
    }

    auto const type = check_expr(scope, value);
    if (type != error_type && scope.result != error_type && type != scope.result)
        report(diagnostic_kind::type_mismatch, file, span_of(file, value),
               cc::format("expected {}, got {}", out.name_of(scope.result), out.name_of(type)));
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
        [&](ast::leading_dot const&) { return not_yet("a leading-dot name"); },
        [&](ast::index const&) { return not_yet("a subscript or type arguments"); },
        [&](ast::tuple const&) { return not_yet("a tuple"); }, [&](ast::array const&) { return not_yet("an array"); },
        [&](ast::object const&) { return not_yet("an object with no struct to convert to"); },
        [&](ast::comparison_chain const&) { return not_yet("a comparison chain"); }, [&](ast::cast const&)
        { return not_yet("as"); }, [&](ast::membership const&) { return not_yet("in"); }, [&](ast::ascription const&)
        { return not_yet("a type ascription"); }, [&](ast::range const&) { return not_yet("a range"); },
        [&](ast::lambda const&) { return not_yet("a lambda"); }, [&](ast::case_expr const&) { return not_yet("case"); },
        [&](ast::loop_expr const&) { return not_yet("loop"); }, [&](ast::return_expr const&)
        { return not_yet("return as a value"); }, [&](ast::yield_expr const&) { return not_yet("yield"); },
        [&](ast::break_expr const&) { return not_yet("break"); }, [&](ast::continue_expr const&)
        { return not_yet("continue"); }, [&](ast::struct_type const&) { return not_yet("a type as a value"); },
        [&](ast::function_type const&) { return not_yet("a type as a value"); },
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
        unsupported(file, where, "an integer literal, which the pass does not type yet");
        return error_type;
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
    return type_of_builtin(builtin::scalar_float, file, where);
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
            return out.at(out.bindings[out.at(binding).info].members)[index].type;
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

    if (call.is_short_circuit)
    {
        // An eager flat call would evaluate the second operand, which `and` and `or` must not.
        (void)check_arguments(scope, call.arguments, false);
        unsupported(file, where, "a short-circuit operator");
        return error_type;
    }

    if (sgl::is_valid(call.op))
    {
        auto const spelling = text_of(file, file_of(file).at(call.op).where);
        auto const arguments = check_arguments(scope, call.arguments, false);
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
    if (out.at(chosen).intrinsic == builtin::none)
        unsupported(file, where, "a call of a function that is no @builtin, which needs the inliner");
    return out.functions[out.at(chosen).info].result;
}
