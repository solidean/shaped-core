#include <shaped-graphics-language/check/impl/expr_helpers.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;
} // namespace

// ---- calls ----------------------------------------------------------------------------------------------------------

call_arguments checker::check_arguments(function_scope& scope, ast::range_of<ast::argument> range, bool is_constructor)
{
    auto const file = scope.file;
    auto result = call_arguments();
    // what the call's own result is expected to be says nothing of its arguments
    auto const outer_expected = expected_result;
    expected_result = type_id::none;
    struct restore
    {
        type_id& at;
        type_id value;
        ~restore() { at = value; }
    } const restored{.at = expected_result, .value = outer_expected};

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

        // CHK-341: the prelude's `undefined()` has the type of the parameter it meets
        if (auto const* const u = !a.is_splat && !a.is_mut && value != nullptr ? value->try_as<ast::call>() : nullptr;
            u != nullptr && is_prelude_file(file) && is_named(file, u->callee, "undefined")
            && ast_of(file).at(u->arguments).empty())
        {
            add_argument(result, {.expr = a.value}, type_id::none, text_of(file, a.name));
            result.undefineds.back() = true;
            set_target(file, a.value, {.kind = target_kind::undefined_value});
            continue;
        }

        // CHK-152: a leading dot is a case of the enum its parameter is, so it has no type until one meets it
        if (auto const* const dot
            = !a.is_splat && !a.is_mut && value != nullptr ? value->try_as<ast::leading_dot>() : nullptr)
        {
            add_argument(result, {.expr = a.value}, type_id::none, text_of(file, a.name));
            result.leading_dots.back() = text_of(file, dot->name);
            continue;
        }

        // CHK-318: a lambda, or the name of a function, has no type until a parameter of function type meets it
        if (!a.is_splat && !a.is_mut && value != nullptr && value->is<ast::lambda>())
        {
            function_arguments.push_back({.expr = a.value, .is_lambda = true});
            add_argument(result, {.expr = a.value, .is_function = true}, type_id::none, text_of(file, a.name), {}, -1,
                         i32(function_arguments.size() - 1));
            continue;
        }
        if (auto const* const n = value != nullptr && !a.is_splat && !a.is_mut ? value->try_as<ast::name>() : nullptr;
            n != nullptr && scope.find_local(text_of(file, n->where)) == nullptr)
        {
            auto const* const found = seen_from(file, text_of(file, n->where));
            if (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::function)
            {
                auto functions = cc::vector<symbol_id>();
                for (auto const f : *found)
                    if (out.at(f).kind == symbol_kind::function)
                        functions.push_back(f);
                function_arguments.push_back({.expr = a.value, .functions = cc::move(functions)});
                add_argument(result, {.expr = a.value, .is_function = true}, type_id::none, text_of(file, a.name), {},
                             -1, i32(function_arguments.size() - 1));
                continue;
            }
        }

        handed.push_back(a.value);
        auto const type = check_expr(scope, a.value);
        handed.remove_back();
        if (type == error_type)
        {
            result.is_poisoned = true;
            continue;
        }
        // a parameter of function type handed on to another stands for the function it was handed (CHK-318)
        if (!a.is_splat && !a.is_mut && out.at(type).kind == type_kind::function)
        {
            add_argument(result, {.expr = a.value, .is_function = true}, type, text_of(file, a.name));
            continue;
        }
        if (a.is_mut)
        {
            // CHK-316: `mut x` hands over a place, so what it names must be one a body could assign
            if (a.is_splat || !judge_place(scope, a.value, "a mut argument"))
                result.is_poisoned = true;
            add_argument(result, {.expr = a.value, .is_mut = true}, type, text_of(file, a.name));
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

namespace
{
/// `a op b` over two folded number literals, in 64 bits: an integer where both are, and a float where either is.
/// None where the language leaves it to a call: an integer `/` or `%` (CHK-313), a float `%`, a bit operator on a
/// float, a shift by a count outside the bits of an int (CHK-270), and an integer result past 64 bits.
number_literal folded(cc::string_view op, number_literal const& a, number_literal const& b)
{
    if (a.is_past_long || b.is_past_long)
        return {};
    if (a.is_integer && b.is_integer)
    {
        auto result = i64(0);
        auto const x = a.integer;
        auto const y = b.integer;
        // past 2^62 a sum may leave 64 bits, and the product's magnitude is judged in a double, which holds it to 2^-52
        constexpr auto limit = i64(1) << 62;
        auto const is_small = [&](i64 v) { return v > -limit && v < limit; };
        auto overflows = false;
        if (op == "+" || op == "-")
        {
            overflows = !is_small(x) || !is_small(y);
            if (!overflows)
                result = op == "+" ? x + y : x - y;
        }
        else if (op == "*")
        {
            auto const product = f64(x) * f64(y);
            overflows = product >= f64(limit) || product <= -f64(limit);
            if (!overflows)
                result = x * y;
        }
        else if (op == "&")
            result = x & y;
        else if (op == "|")
            result = x | y;
        else if (op == "^")
            result = x ^ y;
        else if ((op == "<<" || op == ">>") && y >= 0 && y <= 31)
            result = op == "<<" ? i64(u64(x) << y) : x >> y;
        else
            return {};
        if (overflows || (op == "<<" && (result >> y) != x))
            return {};
        return {.is_number = true, .is_integer = true, .integer = result};
    }
    auto const x = a.is_integer ? f64(a.integer) : a.real;
    auto const y = b.is_integer ? f64(b.integer) : b.real;
    auto result = 0.0;
    if (op == "+")
        result = x + y;
    else if (op == "-")
        result = x - y;
    else if (op == "*")
        result = x * y;
    else if (op == "/" && y != 0.0)
        result = x / y;
    else
        return {};
    return {.is_number = true, .real = result};
}
} // namespace

bool checker::calls_prelude_operator(i32 file, ast::expr_id expr) const
{
    // a program's own operator is a function it calls, which folding would step past
    auto const& t = tables_of(file).target_at(expr);
    return t.kind == target_kind::overload && is_valid(t.symbol) && is_valid(out.at(t.symbol).intrinsic)
        && is_prelude_file(out.at(t.symbol).file);
}

cc::optional<bool> checker::comparison_of(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return cc::nullopt;
    auto const* const call = ast_of(file).at(expr).node.try_as<ast::call>();
    if (call == nullptr || call->spelling != ast::call_spelling::infix || !sgl::is_valid(call->op)
        || call->is_short_circuit || call->arguments.count != 2 || !calls_prelude_operator(file, expr))
        return cc::nullopt;
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast_of(file).at(call->arguments);
    for (auto const& argument : arguments)
        if (!argument.name.empty() || argument.is_splat)
            return cc::nullopt;
    auto const a = number_of(file, arguments[0].value);
    auto const b = number_of(file, arguments[1].value);
    if (!a.is_number || !b.is_number)
        return cc::nullopt;
    if (a.is_integer && b.is_integer)
    {
        auto const x = a.integer;
        auto const y = b.integer;
        if (op == "==")
            return x == y;
        if (op == "!=")
            return x != y;
        if (op == "<")
            return x < y;
        if (op == "<=")
            return x <= y;
        if (op == ">")
            return x > y;
        if (op == ">=")
            return x >= y;
        return cc::nullopt;
    }
    auto const x = a.is_integer ? f64(a.integer) : a.real;
    auto const y = b.is_integer ? f64(b.integer) : b.real;
    if (op == "==")
        return x == y;
    if (op == "!=")
        return x != y;
    if (op == "<")
        return x < y;
    if (op == "<=")
        return x <= y;
    if (op == ">")
        return x > y;
    if (op == ">=")
        return x >= y;
    return cc::nullopt;
}

number_literal checker::number_of(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return {};
    // CHK-394: an operator over number literals alone is a number literal itself
    if (auto const* const call = ast_of(file).at(expr).node.try_as<ast::call>())
    {
        if (!sgl::is_valid(call->op) || call->is_short_circuit || !calls_prelude_operator(file, expr))
            return {};
        auto const op = text_of(file, file_of(file).at(call->op).where);
        auto const arguments = ast_of(file).at(call->arguments);
        for (auto const& a : arguments)
            if (!a.name.empty() || a.is_splat)
                return {};
        if (call->spelling == ast::call_spelling::prefix && arguments.size() == 1)
        {
            auto const operand = number_of(file, arguments[0].value);
            if (!operand.is_number || operand.is_past_long)
                return {};
            if (op == "-" && operand.is_integer && operand.integer != i64(1ull << 63))
                return {.is_number = true, .is_integer = true, .integer = -operand.integer};
            if (op == "-" && !operand.is_integer)
                return {.is_number = true, .real = -operand.real};
            if (op == "~" && operand.is_integer)
                return {.is_number = true, .is_integer = true, .integer = ~operand.integer};
            return {};
        }
        if (call->spelling != ast::call_spelling::infix || arguments.size() != 2)
            return {};
        auto const a = number_of(file, arguments[0].value);
        if (!a.is_number)
            return {};
        auto const b = number_of(file, arguments[1].value);
        if (!b.is_number)
            return {};
        return folded(op, a, b);
    }
    if (!ast_of(file).at(expr).node.is<ast::literal>())
        return {};
    auto const text = text_of(file, span_of(file, expr));
    switch (classify_number(text))
    {
    case number_class::plain_integer:
    {
        if (auto const past_long = parse_literal_past_long(text); past_long.has_value())
            return {.is_number = true, .is_integer = true, .integer = i64(past_long.value()), .is_past_long = true};
        auto const value = parse_literal_integer(text);
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
    // CHK-357: a suffix names the literal's type, so it converts to no other
    case number_class::suffixed:
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

cc::string checker::call_text(i32 file_of_call, cc::string_view spelling, call_arguments const& arguments) const
{
    auto text = cc::string(spelling);
    text += "(";
    for (auto i = isize(0); i < arguments.types.size(); ++i)
    {
        if (i > 0)
            text += ", ";
        if (!arguments.names[i].empty())
            text.appendf("{} = ", arguments.names[i]);
        // a tuple or an object literal has no type until the parameter it meets gives it one, and a number literal
        // is named by its text, since its default type is not what failed to match
        if (arguments.literals[i] >= 0)
            text += "a literal";
        else if (arguments.undefineds[i])
            text += "undefined()";
        else if (!arguments.leading_dots[i].empty())
            text.appendf(".{}", arguments.leading_dots[i]);
        else if (arguments.functions[i] >= 0)
            text += function_arguments[arguments.functions[i]].is_lambda ? "a lambda" : "a function";
        else if (arguments.numbers[i].is_number && ast::is_valid(arguments.written[i].expr))
            text += text_of(file_of_call, span_of(file_of_call, arguments.written[i].expr));
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
        if (spelling == "==" || spelling == "!=")
            if (auto const compared = check_type_comparison(scope, id, call); compared.has_value())
                return compared.value();
        auto arguments = check_arguments(scope, call.arguments, false);
        // CHK-152: a leading dot beside an enum, `api == .vulkan`, names a case of that enum
        if ((spelling == "==" || spelling == "!=") && arguments.types.size() == 2)
            for (auto k = isize(0); k < 2; ++k)
            {
                auto const enumeration = arguments.types[1 - k];
                if (arguments.leading_dots[k].empty() || !is_valid(enumeration) || !out.is_plain_enum(enumeration))
                    continue;
                auto const name = arguments.leading_dots[k];
                auto const dot = arguments.written[k].expr;
                auto const cases = out.at(out.at(enumeration).cases);
                auto index = -1;
                for (auto i = isize(0); i < cases.size(); ++i)
                    if (cases[i].name == name)
                        index = i32(i);
                if (index < 0)
                {
                    report(diagnostic_kind::unknown_member, file, span_of(file, dot),
                           cc::format("the enum {} has no case {}", out.name_of(enumeration), name));
                    return error_type;
                }
                set_target(file, dot,
                           {.kind = target_kind::enum_case, .symbol = out.at(enumeration).symbol, .index = index});
                set_type(file, dot, enumeration);
                arguments.types[k] = enumeration;
                arguments.leading_dots[k] = {};
            }
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
        // CHK-513: `==` and `!=` of two arrays of one type compare every element, unless the program declared its own
        if ((spelling == "==" || spelling == "!=") && arguments.types.size() == 2
            && arguments.types[0] == arguments.types[1] && out.at(arguments.types[0]).kind == type_kind::array)
        {
            type_id const both[] = {arguments.types[0], arguments.types[1]};
            auto element = arguments.types[0];
            while (out.at(element).kind == type_kind::array)
                element = out.at(element).element;
            auto const compared = out.is_plain_enum(element) ? type_of_builtin(builtins::k_int, file, where) : element;
            type_id const elements[] = {compared, compared};
            if (!is_valid(find_operator(file, spelling, both)) && is_valid(find_operator(file, "==", elements)))
                return type_of_builtin(builtins::k_bool, file, where);
        }
        auto const found = operators_of(file, spelling);
        auto const result = resolve_overload(scope, id, ast::expr_id::none, cc::span<symbol_id const>(found), arguments,
                                             cc::format("operator {}", spelling));
        // CHK-394: what the operator folds to is judged at the type it ends up with, as a literal written there is
        if (result != error_type && number_of(file, id).is_number)
            wide_literals.push_back({.file = file, .expr = id, .function = scope.function, .instance = current_instance});
        return result;
    }

    if (!ast::is_valid(call.callee))
        return error_type;

    auto const& callee = ast.at(call.callee);
    auto const callee_where = span_of(file, call.callee);
    auto const* const n = callee.node.try_as<ast::name>();
    if (callee.node.is<ast::member>())
        return check_dot_call(scope, id, call);
    // CHK-344: `table[i](mut p)`, a callable of a table picked at run time
    if (auto const* const index = callee.node.try_as<ast::index>())
        if (auto const table = callables_named(file, index->object); is_valid(table))
            return check_callable_call(scope, id, call, *index, table);
    // CHK-340: `f[float, .B = int](x)` states generic arguments of a function, each a type, by position or by name
    if (auto const* const index = callee.node.try_as<ast::index>())
        if (auto const* const object = ast.at(index->object).node.try_as<ast::name>(); object != nullptr)
        {
            auto const name = text_of(file, object->where);
            auto const* const found = seen_from(file, name);
            auto const is_function = scope.find_local(name) == nullptr && !is_type_parameter_name(name)
                                  && found != nullptr && !found->empty()
                                  && out.at(found->front()).kind == symbol_kind::function;
            // PROTOTYPE (autodiff): `tangent[T]()` is the zero of that tangent type, whose fields are then assigned
            if (name == "tangent" && scope.find_local(name) == nullptr && (found == nullptr || found->empty()))
            {
                auto const type = resolve_value_type(file, call.callee, &scope);
                if (!ast.at(call.arguments).empty())
                {
                    (void)check_arguments(scope, call.arguments, false);
                    if (type != error_type)
                        unsupported(file, callee_where,
                                    "a tangent built from its fields: `tangent[T]()` is its zero, whose fields are "
                                    "then "
                                    "assigned");
                    return error_type;
                }
                if (type != error_type)
                    set_target(file, id, {.kind = target_kind::zero_value});
                return type;
            }
            // CHK-339: `ring[4, float](…)` constructs that instance, which binds every parameter its constructor has
            if (scope.find_local(name) == nullptr && !is_type_parameter_name(name)
                && is_valid(generic_named(file, index->object)))
            {
                auto const instance = resolve_value_type(file, call.callee, &scope);
                if (instance == error_type)
                {
                    (void)check_arguments(scope, call.arguments, false);
                    return error_type;
                }
                auto const outer_expected = expected_result;
                expected_result = instance;
                auto const result = check_named_call(scope, id, call, name, found, false);
                expected_result = outer_expected;
                return result;
            }
            if (is_function)
            {
                auto stated = cc::vector<stated_argument>();
                auto is_sound = true;
                for (auto const& a : ast.at(index->arguments))
                {
                    auto const type = stated_argument_of(scope, a.value);
                    is_sound = is_sound && type != error_type && !a.is_splat;
                    stated.push_back({.name = a.name.empty() ? cc::string_view() : text_of(file, a.name), .type = type});
                }
                if (!is_sound)
                {
                    (void)check_arguments(scope, call.arguments, false);
                    return error_type;
                }
                set_target(file, index->object, {.kind = target_kind::symbol, .symbol = found->front()});
                return check_named_call(scope, id, call, name, found, false, stated);
            }
        }
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
    if (auto const* const local = scope.find_local(text); local != nullptr && !local->is_captured
                                                          && local->type != error_type && is_valid(local->type)
                                                          && out.at(local->type).kind == type_kind::function)
    {
        // by value: checking the arguments may declare locals, and `locals` then moves
        auto const through = *local;
        return check_function_call(scope, id, call, through);
    }
    if (auto const* const local = scope.find_local(text))
    {
        set_target(file, call.callee, local->where);
        (void)check_arguments(scope, call.arguments, false);
        if (local->is_captured)
        {
            report_capture(scope, callee_where, *local);
            return error_type;
        }
        // a local hides every symbol of its name, which a port meets as a parameter named like a builtin
        auto hidden = cc::string_view();
        if (auto const* const found = seen_from(file, text))
            for (auto const s : *found)
                if (out.at(s).kind == symbol_kind::function || out.at(s).kind == symbol_kind::structure)
                    hidden = is_prelude_file(out.at(s).file) ? "the prelude's" : "the module's";
        if (hidden.empty())
            unsupported(file, callee_where, "a call of a local value");
        else
            unsupported(file, callee_where,
                        cc::format("a call of a local value: the {} {} hides {} {}, so rename the {}",
                                   local->where.kind == target_kind::parameter ? "parameter" : "local", text, hidden,
                                   text, local->where.kind == target_kind::parameter ? "parameter" : "local"));
        return error_type;
    }

    // CHK-338: a type parameter hides every symbol of its name, and a call of it is a call of what it stands for in
    // each instance, which its generic pass leaves to them
    if (is_type_parameter_name(text))
    {
        auto stands_for = type_id::none;
        for (auto i = type_parameter_names.size(); i > 0 && !is_valid(stands_for); --i)
            if (type_parameter_names[i - 1].first == text)
                stands_for = type_parameter_names[i - 1].second;
        if (current_instance >= 0 && is_valid(stands_for) && !is_open(stands_for) && is_valid(out.at(stands_for).symbol))
        {
            auto const name = out.at(out.at(stands_for).symbol).name;
            return check_named_call(scope, id, call, name, seen_from(file, name), false);
        }
        (void)check_arguments(scope, call.arguments, false);
        if (generic_passes == 0)
            report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
                   cc::format("{} is a type parameter, and a call needs a function or a struct", text));
        return error_type;
    }

    // CHK-329: a trace whose third argument names a ray type is a ray-tracing stage's trace
    // CHK-434: `trace_hit` is one that gives its hit object rather than running its shaders
    if (text == "trace" || text == "trace_hit")
    {
        auto const arguments = ast.at(call.arguments);
        if (arguments.size() >= 4 && arguments[2].name.empty() && ast::is_valid(arguments[2].value))
            if (auto ray = ray_type_of(file, arguments[2].value); ray.has_value())
            {
                if (text == "trace_hit")
                    ray.value().kind = ray_trace_kind::hit_object;
                return ray.value().ray < 0 ? checked_module::error_type
                                           : check_pipeline_trace(scope, id, call, ray.value());
            }
    }

    // CHK-485: a task stage's launch, whose payload is a struct of the program
    if (text == "dispatch_mesh")
        return check_dispatch_mesh(scope, id, call);

    return check_named_call(scope, id, call, text, seen_from(file, text), false);
}

type_id checker::check_named_call(function_scope& scope,
                                  ast::expr_id id,
                                  ast::call const& call,
                                  cc::string_view text,
                                  cc::vector<symbol_id> const* found,
                                  bool is_qualified,
                                  cc::span<stated_argument const> stated)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const callee_where = span_of(file, call.callee);

    // CHK-247: the functions of its name, and those of its first argument's type scope, whatever else the name is
    auto const kind = found == nullptr || found->empty() ? symbol_kind::function : out.at(found->front()).kind;
    if (kind == symbol_kind::alias)
    {
        // CHK-14: a call of an alias is a call of the struct it names, whose overload set is seen where it is declared
        auto const alias = found->front();
        set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = alias});
        auto const named = declared_type_of(alias, file, callee_where);
        if (!is_valid(named) || out.at(named).kind != symbol_kind::structure)
        {
            (void)check_arguments(scope, call.arguments, false);
            if (out.at(alias).state == symbol_state::checked)
                report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
                       cc::format("{} names {}, and a call needs a function or a struct", text,
                                  out.name_of(out.at(alias).type)));
            return error_type;
        }
        auto const& struct_name = out.at(named).name;
        auto arguments = check_arguments(scope, call.arguments, true);
        arguments.stated = cc::vector<stated_argument>::create_copy_of(stated);
        auto const first_type
            = arguments.written.empty() || arguments.written[0].splat_member > 0 ? type_id::none : arguments.types[0];
        auto const candidates = candidates_of(out.at(named).file, struct_name, first_type);
        if (demand(named, file, callee_where) != symbol_state::checked)
            return error_type;
        auto const type = out.at(named).type;
        if (candidates.empty())
        {
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{} is opaque and has no constructor", out.name_of(type)));
            return type;
        }
        auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, struct_name);
        return result == error_type ? type : result;
    }
    auto const is_structure = kind == symbol_kind::structure;
    auto arguments = check_arguments(scope, call.arguments, is_structure);
    arguments.stated = cc::vector<stated_argument>::create_copy_of(stated);
    auto const first_type
        = arguments.written.empty() || arguments.written[0].splat_member > 0 ? type_id::none : arguments.types[0];
    // CHK-348: `m.f(…)` names the functions of `m`, and no type scope widens them
    auto candidates = cc::vector<symbol_id>();
    if (!is_qualified)
        candidates = candidates_of(file, text, first_type);
    else if (found != nullptr)
        for (auto const candidate : *found)
            if (out.at(candidate).kind == symbol_kind::function)
                candidates.push_back(candidate);

    if (is_structure)
    {
        // CHK-75: a call of a struct's name is a call of its overload set, its synthesized constructor among it.
        auto const first = found->front();
        if (demand(first, file, callee_where) != symbol_state::checked)
            return error_type;
        auto const type = out.at(first).type;
        if (candidates.empty())
        {
            set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{} is opaque and has no constructor", out.name_of(type)));
            return type;
        }
        auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, text);
        // A call that names a struct is of that struct, so one bad argument does not take the whole value with it.
        return result == error_type ? type : result;
    }

    if (kind != symbol_kind::function && candidates.empty())
    {
        auto const first = found->front();
        set_target(file, call.callee, {.kind = target_kind::symbol, .symbol = first});
        if (kind == symbol_kind::binding)
            report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
                   cc::format("{} is a binding, and a call needs a function or a struct", text));
        else
            report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
                   cc::format("{} is {}, and a call needs a function or a struct", text,
                              kind == symbol_kind::pipeline   ? "a pipeline"
                              : kind == symbol_kind::constant ? "a const"
                              : kind == symbol_kind::sampler  ? "a sampler"
                                                              : "an enum"));
        return error_type;
    }
    if (candidates.empty())
    {
        if (!is_qualified && used_module(file, text) >= 0)
            report(diagnostic_kind::wrong_kind_of_name, file, callee_where,
                   cc::format("{} is a module, and a call needs a function or a struct", text));
        else
            report(diagnostic_kind::unknown_name, file, callee_where, text_of(file, callee_where));
        return error_type;
    }
    if (!judge_uniform_load(file, candidates, call.arguments))
        return error_type;
    // PROTOTYPE (autodiff): a derivative of a named function takes what that function's parameters ask
    auto const outer_derivative = is_resolving_derivative;
    if ((text == "fwd_diff" || text == "bwd_diff") && is_prelude_file(out.at(candidates[0]).file))
        if (auto derived = derivative_candidates(candidates, arguments, file, callee_where); !derived.empty())
        {
            candidates = cc::move(derived);
            is_resolving_derivative = true;
        }
    auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, text);
    is_resolving_derivative = outer_derivative;
    if (result != error_type)
    {
        judge_filtering(file, id, where, arguments.written);
        judge_constant_arguments(file, id);
    }
    return result;
}

bool checker::judge_uniform_load(i32 file, cc::span<symbol_id const> candidates, ast::range_of<ast::argument> arguments)
{
    auto is_load = !candidates.empty();
    for (auto const c : candidates)
    {
        auto const* const record = out.builtin_function(out.at(c).intrinsic);
        is_load = is_load && record != nullptr && record->is_uniform_load;
    }
    auto const& ast = ast_of(file);
    auto const written = ast.at(arguments);
    // a call of another shape matches no overload, which resolution says
    if (!is_load || written.size() != 1 || !written[0].name.empty() || written[0].is_splat
        || !ast::is_valid(written[0].value))
        return true;
    auto const tables = tables_of(file);
    auto const refuse = [&](ast::expr_id at)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, at),
               "workgroup_uniform_load reads a member of a @workgroup binding, or a field of one");
        return false;
    };
    // the path down to the member it starts from, which decides whether the argument is workgroup memory at all
    auto index = ast::expr_id::none;
    auto component = ast::expr_id::none;
    for (auto expr = written[0].value; ast::is_valid(expr);)
    {
        auto const& node = ast.at(expr).node;
        if (auto const* const i = node.try_as<ast::index>())
        {
            index = expr;
            expr = i->object;
            continue;
        }
        auto const* const member = node.try_as<ast::member>();
        if (member == nullptr)
            return refuse(written[0].value);
        auto const& where = tables.target_at(expr);
        if (where.kind == target_kind::binding_member)
        {
            auto const& binding = out.bindings[out.at(where.symbol).info];
            auto const type = tables.type_at(expr);
            if (!binding.is_workgroup || (is_valid(type) && out.at(type).kind == type_kind::atomic))
                return refuse(written[0].value);
            if (ast::is_valid(index))
                unsupported(file, span_of(file, index), "an index on the way to the memory workgroup_uniform_load reads");
            // WGSL takes no pointer to a vector's component
            else if (ast::is_valid(component))
                unsupported(file, span_of(file, component), "a component of a vector, read by workgroup_uniform_load");
            return !ast::is_valid(index) && !ast::is_valid(component);
        }
        if ((where.kind == target_kind::field || where.kind == target_kind::swizzle)
            && out.builtin_type_of(tables.type_at(member->object)) != nullptr)
            component = expr;
        else if (where.kind != target_kind::field)
            return refuse(written[0].value);
        expr = member->object;
    }
    return refuse(written[0].value);
}

type_id checker::check_dot_call(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& member = ast.at(call.callee).node.as<ast::member>();
    auto const name = text_of(file, member.name);

    // CHK-289: `T[N].filled(v)`, where `T[N]` is a type in the position of a value
    if (auto const* const applied
        = ast::is_valid(member.object) ? ast.at(member.object).node.try_as<ast::index>() : nullptr;
        applied != nullptr && ast::is_valid(applied->object) && is_type_name(file, applied->object)
        && (!ast.at(applied->object).node.is<ast::name>()
            || scope.find_local(text_of(file, span_of(file, applied->object))) == nullptr))
        return check_filled(scope, id, call);

    // CHK-348: `m.foo(…)` is a call of the name `foo` of module `m`, and `m` is no argument
    if (module_named(file, member.object, &scope) >= 0)
        return check_named_call(scope, id, call, name, symbols_named(file, call.callee, &scope), true);

    // `T.foo(…)`: the functions of the type scope of `T`, and `T` is no argument (CHK-248)
    if (auto const* const found = symbols_named(file, member.object, &scope))
    {
        // an alias of a struct or an enum reaches its type scope as the type's own name does (CHK-14)
        auto const owner = declared_type_of(found->front(), file, span_of(file, member.object));
        if (is_valid(owner))
        {
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = found->front()});
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
        if (out.at(found->front()).kind == symbol_kind::binding)
        {
            (void)check_arguments(scope, call.arguments, false);
            unsupported(file, span_of(file, call.callee), "a call through a binding");
            return error_type;
        }
    }

    // `a.foo(…)` is `foo(a, …)`, and a field of `a` takes part in no call (CHK-249)
    handed.push_back(member.object);
    auto const receiver = check_expr(scope, member.object);
    handed.remove_back();
    if (receiver != error_type && out.at(receiver).kind == type_kind::stream
        && (out.at(receiver).count > 0 || name == "vertex" || name == "primitive"))
        return check_stream_call(scope, id, call, receiver);
    // CHK-482: what a primitive is follows from the entry point's `@mesh`, which writes lines or triangles
    if (receiver != error_type && out.at(receiver).kind == type_kind::stream && (name == "line" || name == "triangle")
        && is_valid(scope.function) && out.at(scope.function).info >= 0)
    {
        auto const is_lines = out.functions[out.at(scope.function).info].is_line_mesh;
        if (is_lines != (name == "line"))
        {
            report(diagnostic_kind::wrong_kind_of_name, file, member.name,
                   is_lines ? cc::string("this mesh stage writes lines, so a primitive is `o.line(i, l)`")
                            : cc::string("this mesh stage writes triangles, so a primitive is `o.triangle(i, t)`; "
                                         "`@mesh(…, lines = M)` writes lines"));
            return error_type;
        }
    }
    if (receiver != error_type && name == "invoke" && out.name_of(receiver) == "hit_object"
        && out.builtin_type_of(receiver) != nullptr)
        return check_hit_invoke(scope, id, call);
    auto arguments = check_arguments(scope, call.arguments, false);
    if (receiver == error_type || member.name.empty())
        return error_type;
    // CHK-373: an `@atomic` image's texels are read and written by their atomics alone
    if (out.at(receiver).kind == type_kind::image && out.at(receiver).is_atomic && (name == "load" || name == "store"))
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, id),
               cc::format("the texels of an @atomic image are atomics, so `{0}` is the texel's: `img[xy].{0}(…)`", name));
        return error_type;
    }
    // CHK-416: a receiver that is a place may stand for a `mut` first parameter, which the call then needs no mark for
    auto quiet = cc::string();
    auto const is_place = judge_place(scope, member.object, "", &quiet);
    arguments.written.insert_at(0, {.expr = member.object, .may_be_mut = is_place});
    arguments.types.insert_at(0, receiver);
    arguments.names.insert_at(0, {});
    // CHK-250, CHK-253: the receiver is argument 0, and a number literal there converts like one anywhere else
    arguments.numbers.insert_at(0, number_of(file, member.object));
    arguments.literals.insert_at(0, -1);
    arguments.functions.insert_at(0, -1);
    arguments.undefineds.insert_at(0, false);
    arguments.leading_dots.insert_at(0, {});

    auto const candidates = candidates_of(file, name, receiver);
    if (candidates.empty())
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}", out.name_of(receiver), name));
        return error_type;
    }
    auto const result = resolve_overload(scope, id, call.callee, candidates, arguments, name, call_spelling::dot_call);
    if (result != error_type)
    {
        judge_filtering(file, id, span_of(file, id), arguments.written);
        judge_constant_arguments(file, id);
    }
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
        if (auto converted = converted_for_plain_vector(file, where, candidates, arguments); converted.has_value())
            return resolve_overload(scope, id, callee, candidates, converted.value(), spelling, written_as);
        note_near_misses(file, id, candidates, arguments);
        // A struct's one constructor says what it takes, which is what a reader needs to fix the call.
        // The functions of the struct's name beside it, such as `float3(x)`, change nothing about that.
        auto constructor = symbol_id::none;
        auto constructor_count = 0;
        for (auto const candidate : candidates)
            if (out.at(candidate).role == function_role::constructor)
            {
                constructor = candidate;
                ++constructor_count;
            }
        if (constructor_count == 1)
        {
            auto expected = cc::vector<type_id>();
            for (auto const& p : out.at(out.functions[out.at(constructor).info].parameters))
                expected.push_back(p.type);
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{}, and the constructor is {}", call_text(file, spelling, arguments),
                              signature_text(spelling, expected)));
        }
        else
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("{}{}", call_text(file, spelling, arguments),
                              miss_hint(scope, callee, candidates, arguments, spelling)));
        return error_type;
    }
    auto const best = best_of(cc::move(matches));
    if (best.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where,
               cc::format("{} has {} candidates", call_text(file, spelling, arguments), best.size()));
        return error_type;
    }

    auto const& chosen_match = best.front();
    auto const chosen = chosen_match.candidate;
    auto const& chosen_symbol = out.at(chosen);

    // CHK-257: an operator over integer literals alone takes no operator of another type, since which one it meets
    // would change the day the prelude gains one of their own
    auto is_integer_literals = spelling.starts_with("operator ") && !arguments.written.empty();
    for (auto const& n : arguments.numbers)
        is_integer_literals = is_integer_literals && n.is_number && n.is_integer;
    auto is_converted = false;
    for (auto const c : chosen_match.chains)
        is_converted = is_converted || c > 0;
    if (is_integer_literals && is_converted)
    {
        report(diagnostic_kind::literal_needs_type, file, where,
               cc::format("{} takes no int, and a literal alone says nothing of which type is meant; write a float "
                          "literal such as 1.0, or `as` a type",
                          spelling));
        return error_type;
    }
    // CHK-270: a shift by a literal count outside the bits of an int can only be a mistake, as WGSL has it, and
    // CHK-452: of a long or a ulong, outside its 64 bits
    auto const shift_bits = [&]
    {
        auto const* const record = out.builtin_type_of(out.functions[chosen_symbol.info].result);
        return record != nullptr && is_64_bit(record->leaf_kind) ? i64(64) : i64(32);
    };
    if ((spelling == "operator <<" || spelling == "operator >>") && arguments.numbers.size() == 2
        && arguments.numbers[1].is_number && arguments.numbers[1].is_integer
        && (arguments.numbers[1].integer < 0 || arguments.numbers[1].integer >= shift_bits()))
    {
        report(diagnostic_kind::shift_out_of_range, file, where,
               cc::format("a shift by {} moves every bit out of {}; a count is 0 to {}", arguments.numbers[1].integer,
                          shift_bits(), shift_bits() - 1));
        return error_type;
    }
    // CHK-313: `/` and `%` are the operators whose integer and float answers differ, so over integer literals alone
    // `1 / 3` would be a silent 0 that literal folding is meant to refuse; the literal says which is meant instead
    if (is_integer_literals && (spelling == "operator /" || spelling == "operator %"))
    {
        report(diagnostic_kind::literal_needs_type, file, where,
               cc::format("{} of integer literals alone says nothing of whether it divides integers or floats; write "
                          "a float literal such as 1.0, or `as` a type",
                          spelling));
        return error_type;
    }
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
    // by value: resolving a literal argument may compile another function, and `functions` then moves
    auto const info = out.functions[chosen_symbol.info];
    auto bindings = chosen_match.bindings;
    commit_literals(scope, arguments, out.at(info.parameters), chosen_match.slots, bindings);
    // CHK-340: what the arguments left unbound, where the call stands says
    auto const result = deduce_result(scope, id, chosen, bindings);
    if (result == error_type)
        return error_type;
    record_call(file, id, chosen, arguments, chosen_match.slots, bindings);
    if (chosen_symbol.role == function_role::constructor)
        set_type(file, callee, result);
    if (is_valid(out.at(chosen).intrinsic))
    {
        // CHK-300: the mark is what an index into a binding array is written with, and on anything else it means nothing
        auto const* const record = out.builtin_function(out.at(chosen).intrinsic);
        if (record != nullptr && record->mark != builtins::index_mark::none && id != binding_index)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, where,
                   cc::format("`{0} i` marks an index into a binding array, and stands only as one: `t[{0} i]`",
                              record->name));
            return error_type;
        }
        return result;
    }

    // A construction is written where it stands, and the defaults of its fields with it, so what they call is reached.
    note_program_call(scope, chosen, where);
    return result;
}

symbol_id checker::function_of_type(cc::span<symbol_id const> candidates, type_id type)
{
    if (!is_valid(type) || type == error_type || out.at(type).kind != type_kind::function)
        return symbol_id::none;
    auto const wanted = out.at(out.at(type).members);
    auto found = symbol_id::none;
    for (auto const f : candidates)
    {
        if (out.at(f).state == symbol_state::in_compilation || demand(f, out.at(f).file, {}) != symbol_state::checked)
            continue;
        auto const& info = out.functions[out.at(f).info];
        // an entry point or a builtin is no function a body calls through a parameter
        if (info.entry_stage != stage::none || is_valid(out.at(f).intrinsic)
            || (info.type_parameters.count == 0 && info.result != out.at(type).element))
            continue;
        auto const parameters = cc::vector<parameter>::create_copy_of(out.at(info.parameters));
        auto is_same = parameters.size() == wanted.size();
        // DIFF-4: a derivative's generic meets the signature of the instance its call deduced
        if (info.type_parameters.count > 0)
        {
            if (!is_resolving_derivative)
                continue;
            auto bindings = cc::vector<type_id>();
            auto const wanted_types = cc::vector<member_info>::create_copy_of(wanted);
            for (auto i = isize(0); is_same && i < parameters.size(); ++i)
                is_same = !parameters[i].is_mut && unify(parameters[i].type, wanted_types[i].type, bindings);
            if (!is_same)
                continue;
            if (is_valid(found))
                return symbol_id::none;
            found = f;
            continue;
        }
        for (auto i = isize(0); is_same && i < parameters.size(); ++i)
            is_same = parameters[i].type == wanted[i].type && !parameters[i].is_mut;
        if (!is_same)
            continue;
        if (is_valid(found))
            return symbol_id::none;
        found = f;
    }
    return found;
}

type_id checker::check_lambda(function_scope& scope, ast::expr_id expr, type_id type, cc::vector<type_id>* bindings)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, expr);
    auto const& l = ast.at(expr).node.as<ast::lambda>();
    // CHK-318: an arrow lambda with an arrow body; the rest of the lambda family is not built
    if (l.spelling != ast::lambda_spelling::arrow || l.body.kind != ast::body_kind::arrow || !ast::is_valid(l.body.value))
    {
        unsupported(file, where, "a lambda other than `x => value`");
        return error_type;
    }
    if (type == error_type || out.at(type).kind != type_kind::function)
        return error_type;
    auto const parameters = cc::vector<member_info>::create_copy_of(out.at(out.at(type).members));
    auto const result = out.at(type).element;
    auto const fields = ast.at(l.parameters);
    // CHK-340: a lambda's parameters are what the call bound, and its result binds what they left; a type parameter
    // of the function it is written in is bound already, to itself
    for (auto const& m : parameters)
    {
        auto named = cc::vector<type_id>();
        collect_type_parameters(m.type, named);
        auto is_in_scope = true;
        for (auto const q : named)
            is_in_scope = is_in_scope && type_parameter_names.values().any([&](auto const& n) { return n.second == q; });
        if (!is_in_scope)
        {
            report(diagnostic_kind::no_matching_overload, file, where,
                   cc::format("this lambda takes {}, which nothing in the call says", out.name_of(m.type)));
            return error_type;
        }
    }
    auto const deduces = bindings != nullptr && is_open(result);
    auto const locals_before = scope.locals.size();
    auto is_sound = true;
    for (auto i = isize(0); i < fields.size(); ++i)
    {
        auto const& p = fields[i];
        if (ast::is_valid(p.type))
        {
            auto const written = resolve_value_type(file, p.type, &scope);
            if (written != error_type && written != parameters[i].type)
            {
                report(diagnostic_kind::type_mismatch, file, span_of(file, p.type),
                       cc::format("this parameter is {}, and the function it stands for takes {}", out.name_of(written),
                                  out.name_of(parameters[i].type)));
                is_sound = false;
            }
        }
        auto const index = isize(&p - ast.fields.data());
        auto const self = target{.kind = target_kind::parameter, .index = i32(index)};
        judge_shadowing(file, text_of(file, p.name), p.name);
        scope.locals.push_back(
            {.name = text_of(file, p.name), .where = self, .type = parameters[i].type, .depth = scope.depth + 1});
    }
    auto const value = deduces ? check_expr(scope, l.body.value) : check_expected(scope, l.body.value, result);
    scope.locals.resize_down_to(locals_before);
    if (!is_sound || value == error_type)
        return error_type;
    if (deduces)
    {
        if (!unify(result, value, *bindings))
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, l.body.value),
                   cc::format("this lambda gives {}, and the function it stands for returns {}", out.name_of(value),
                              out.name_of(result)));
            return error_type;
        }
        auto const deduced = substitute(type, *bindings);
        set_type(file, expr, deduced);
        return deduced;
    }
    if (value != result)
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, l.body.value),
               cc::format("this lambda gives {}, and the function it stands for returns {}", out.name_of(value),
                          out.name_of(result)));
        return error_type;
    }
    set_type(file, expr, type);
    return type;
}

type_id checker::check_function_call(function_scope& scope, ast::expr_id id, ast::call const& call, local_name const& local)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    set_target(file, call.callee, local.where);
    auto const arguments = check_arguments(scope, call.arguments, false);
    if (arguments.is_poisoned)
        return error_type;
    auto const type = local.type;
    auto parameters = cc::vector<parameter>();
    for (auto const& m : out.at(out.at(type).members))
        parameters.push_back({.type = m.type});
    auto const bound = bind_arguments(parameters, arguments);
    auto is_match = bound.failure == miss_reason::none;
    for (auto p = isize(0); is_match && p < parameters.size(); ++p)
        is_match = bound.slots[p] >= 0 && chain_of(file, parameters[p], arguments, bound.slots[p]).has_value();
    if (!is_match)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               cc::format("{} is {}, and these arguments do not fit it", local.name, out.name_of(type)));
        return error_type;
    }
    // CHK-319: the call is written where the function was handed over, so its record names no callee
    record_call(file, id, symbol_id::none, arguments, bound.slots);
    commit_literals(scope, arguments, parameters, bound.slots);
    return out.at(type).element;
}
