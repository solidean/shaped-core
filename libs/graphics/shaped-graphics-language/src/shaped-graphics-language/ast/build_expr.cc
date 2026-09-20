#include <shaped-graphics-language/ast/impl/builder.hh>

using namespace sgl;
using namespace sgl::ast;
using namespace sgl::ast::impl;

expr_id builder::expression(form_id form, attribute_mode mode)
{
    if (mode == attribute_mode::reject)
        reject_attributes(form);
    auto const attributes = mode == attribute_mode::taken ? range_of<attribute>() : attributes_of(form);

    auto const result = expression_node(form);
    // `(x)` hands back the node of `x`, which keeps what was written on `x` itself.
    auto& node = ast.exprs[index_of(result)];
    if (!attributes.empty() && node.attributes.empty())
        node.attributes = attributes;
    return result;
}

expr_id builder::expression_node(form_id form)
{
    auto const& f = at(form);
    switch (f.kind)
    {
    case form_kind::missing:
    case form_kind::error:
    case form_kind::postfix_operator:
        // Reported by the form parser.
        return invalid_expression(form);

    case form_kind::number:
        return make_expr(form, literal{.kind = literal_kind::number});
    case form_kind::quoted:
        return make_expr(form, literal{.kind = literal_kind::quoted});
    case form_kind::hash_literal:
        return make_expr(form, literal{.kind = literal_kind::hash});

    case form_kind::identifier:
        if (text_of(form) == "self")
            return make_expr(form, self_ref{});
        return make_expr(form, name{.where = f.where});
    case form_kind::wildcard:
        return make_expr(form, wildcard{});
    case form_kind::leading_dot:
        return make_expr(form, leading_dot{.name = file.at(f.token).where});
    case form_kind::member:
    {
        auto const object = expression(f.first_child);
        return make_expr(form, member{.object = object, .name = file.at(f.token).where});
    }

    case form_kind::round_list:
        return round_list_expression(form);
    case form_kind::square_list:
    {
        auto const elements = list_elements(form);
        return make_expr(form, array{.elements = elements});
    }
    case form_kind::curly_list:
        return curly_list_expression(form);

    case form_kind::call:
        return call_expression(form);
    case form_kind::application:
        return application_expression(form);
    case form_kind::prefix_operator:
        return prefix_expression(form);
    case form_kind::operator_run:
        return run_expression(form);
    case form_kind::keyword_form:
        return keyword_expression(form);

    case form_kind::keyword:
    case form_kind::op:
        return invalid_expression(form, diagnostic_kind::unexpected_keyword);
    case form_kind::block:
        return invalid_expression(form, diagnostic_kind::unsupported_syntax);
    case form_kind::sequence:
        return invalid_expression(form, diagnostic_kind::statement_in_expression);
    }
    return invalid_expression(form, diagnostic_kind::unsupported_syntax);
}

bool builder::has_comma_after(form_id list, form_id element) const
{
    auto const list_end = at(list).where.end();
    auto const from = at(element).where.end();
    auto depth = 0;
    for (auto t = at(list).token; index_of(t) < file.tokens.size() && file.at(t).where.offset < list_end; t = next(t))
    {
        auto const& token = file.at(t);
        if (token.where.offset < from)
            continue;
        // A trailing attribute may hold commas of its own: `(x @range(0, 1))`.
        switch (token.kind)
        {
        case token_kind::round_open:
        case token_kind::square_open:
        case token_kind::curly_open:
            ++depth;
            break;
        case token_kind::round_close:
        case token_kind::square_close:
        case token_kind::curly_close:
            --depth;
            break;
        case token_kind::comma:
            if (depth == 0)
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

expr_id builder::round_list_expression(form_id form)
{
    auto const first = at(form).first_child;
    auto const is_single = is_valid(first) && !is_valid(at(first).next_sibling);
    auto const is_plain = is_single && !is_binary_run(first, "=")
                       && !(is_kind(first, form_kind::prefix_operator) && token_text_of(first) == "..");
    if (is_plain && !has_comma_after(form, first))
        return expression(first);

    auto const elements = list_elements(form);
    return make_expr(form, tuple{.elements = elements});
}

expr_id builder::curly_list_expression(form_id form)
{
    auto typed = 0;
    auto total = 0;
    for (auto element = at(form).first_child; is_valid(element); element = at(element).next_sibling, ++total)
        if (is_field_like(element, true))
            ++typed;

    if (total > 0 && typed == total)
    {
        auto const fields = fields_of(form, diagnostic_kind::expected_member);
        return make_expr(form, struct_type{.fields = fields});
    }
    if (typed > 0)
        report(diagnostic_kind::mixed_struct_type, form);

    // In a mixed list only the mix is reported: an attribute on a `name: type` element was written on a field.
    auto collected = cc::vector<argument>();
    for (auto element = at(form).first_child; is_valid(element); element = at(element).next_sibling)
    {
        auto const is_field = is_field_like(element, true);
        auto const is_splat = is_kind(element, form_kind::prefix_operator) && token_text_of(element) == "..";
        // Anything else is kept as a positional element, so nothing written is lost.
        if (!is_field && !is_splat && !is_kind(element, form_kind::identifier) && !is_binary_run(element, "="))
            report(diagnostic_kind::expected_object_element, element);
        collected.push_back(list_element(element, true, is_field));
    }
    auto const elements = append(ast.arguments, cc::span<argument const>(collected));
    return make_expr(form, object{.elements = elements});
}

expr_id builder::call_expression(form_id form)
{
    auto const callee_form = at(form).first_child;
    auto const list = at(callee_form).next_sibling;
    auto const target = expression(callee_form);

    switch (at(list).kind)
    {
    case form_kind::square_list:
    {
        auto const arguments = list_elements(list);
        return make_expr(form, index{.object = target, .arguments = arguments});
    }
    case form_kind::curly_list:
    {
        report(diagnostic_kind::unsupported_syntax, list);
        auto const bindings = list_elements(list, true);
        return make_expr(form, with_bindings{.target = target, .bindings = bindings});
    }
    default:
    {
        auto const arguments = list_elements(list);
        return make_expr(form, call{.spelling = call_spelling::paren, .callee = target, .arguments = arguments});
    }
    }
}

expr_id builder::application_expression(form_id form)
{
    auto const head = at(form).first_child;
    auto const callee = expression(head);

    auto collected = cc::vector<argument>();
    for (auto operand = at(head).next_sibling; is_valid(operand); operand = at(operand).next_sibling)
    {
        auto const value = expression(operand);
        collected.push_back({.form = operand, .value = value});
    }
    auto const arguments = append(ast.arguments, cc::span<argument const>(collected));
    return make_expr(form, call{.spelling = call_spelling::juxtaposition, .callee = callee, .arguments = arguments});
}

expr_id builder::prefix_expression(form_id form)
{
    if (token_text_of(form) == "..")
    {
        // The operand is still read, so what is wrong inside it is found in the same run.
        (void)expression(at(form).first_child);
        return invalid_expression(form, diagnostic_kind::misplaced_splat);
    }

    auto const operand = at(form).first_child;
    auto const value = expression(operand);
    auto const arguments = append_one(ast.arguments, argument{.form = operand, .value = value});
    return make_expr(form, call{.spelling = call_spelling::prefix, .op = at(form).token, .arguments = arguments});
}

expr_id builder::infix_call(form_id run, expr_id left, form_id op, expr_id right, bool is_short_circuit)
{
    argument const both[] = {
        {.form = ast.at(left).form, .value = left},
        {.form = ast.at(right).form, .value = right},
    };
    auto const arguments = append(ast.arguments, cc::span<argument const>(both));
    return make_expr(run, call{.spelling = call_spelling::infix,
                               .op = at(op).token,
                               .arguments = arguments,
                               .is_short_circuit = is_short_circuit});
}

expr_id builder::run_expression(form_id form)
{
    auto const parts = run_parts_of(form);
    if (parts.operators.empty() || parts.operators.size() + 1 != parts.operands.size())
        return invalid_expression(form, diagnostic_kind::expected_expression);

    auto const level = level_of(parts.operators[0]);
    switch (level)
    {
    case operator_level::assignment:
        return invalid_expression(form, diagnostic_kind::statement_in_expression);
    case operator_level::computes_as:
        return lambda_expression(form, parts);
    case operator_level::ascription:
        return ascription_fold(form, parts.operands, parts.operators, attribute_mode::reject);
    case operator_level::arrow:
        if (parts.operands.size() != 2)
            return invalid_expression(form, diagnostic_kind::expected_expression);
        return function_type_expression(form, parts.operands[0], parts.operands[1]);

    case operator_level::comparison:
        if (parts.operators.size() > 1)
        {
            auto operands = cc::vector<expr_id>();
            auto operators = cc::vector<token_id>();
            for (auto const operand : parts.operands)
                operands.push_back(expression(operand));
            for (auto const op : parts.operators)
                operators.push_back(at(op).token);
            auto const operand_range = append(ast.expr_lists, cc::span<expr_id const>(operands));
            auto const operator_range = append(ast.token_lists, cc::span<token_id const>(operators));
            return make_expr(form, comparison_chain{.operands = operand_range, .operators = operator_range});
        }
        break;
    default:
        break;
    }

    // Everything else associates to the left: `a - b + c` is `(a - b) + c`.
    auto result = expression(parts.operands[0]);
    for (auto i = isize(0); i < parts.operators.size(); ++i)
    {
        auto const right = expression(parts.operands[i + 1]);
        auto const op = parts.operators[i];
        if (level == operator_level::range)
            result = make_expr(form, range{.first = result, .last = right, .op = at(op).token});
        else
            result = infix_call(form, result, op, right, level == operator_level::connective);
    }
    return result;
}

expr_id builder::ascription_fold(form_id run,
                                 cc::span<form_id const> operands,
                                 cc::span<form_id const> operators,
                                 attribute_mode first_mode)
{
    auto result = expression(operands[0], first_mode);
    for (auto i = isize(0); i < operators.size(); ++i)
    {
        auto const op = token_text_of(operators[i]);
        if (op == "in")
            result = make_expr(run, membership{.value = result, .container = expression(operands[i + 1])});
        else if (op == "as")
            result = make_expr(run, cast{.value = result, .type = type_expression(operands[i + 1])});
        else
            result = make_expr(run, ascription{.value = result, .type = type_expression(operands[i + 1])});
    }
    return result;
}

expr_id builder::function_type_expression(form_id run, form_id left, form_id right)
{
    auto parameters = range_of<field>();
    if (is_kind(left, form_kind::round_list))
    {
        auto collected = cc::vector<field>();
        for (auto e = at(left).first_child; is_valid(e); e = at(e).next_sibling)
        {
            if (is_field_like(e, true))
                collected.push_back(make_field(e, diagnostic_kind::expected_parameter));
            else
            {
                auto const attributes = attributes_of(e);
                auto const type = expression(e, attribute_mode::taken);
                collected.push_back({.form = e, .type = type, .attributes = attributes});
            }
        }
        parameters = append(ast.fields, cc::span<field const>(collected));
    }
    else
    {
        auto const type = type_expression(left);
        parameters = append_one(ast.fields, field{.form = left, .type = type});
    }
    auto const result = type_expression(right);
    return make_expr(run, function_type{.parameters = parameters, .result = result});
}

expr_id builder::type_after_first_operator(form_id run, run_parts const& parts)
{
    if (parts.operators.size() + 1 != parts.operands.size() || parts.operands.size() < 2)
        return invalid_expression(run, diagnostic_kind::expected_expression);
    auto const operands = cc::span<form_id const>(parts.operands);
    auto const operators = cc::span<form_id const>(parts.operators);
    return ascription_fold(run, operands.subspan({.offset = 1, .size = operands.size() - 1}),
                           operators.subspan({.offset = 1, .size = operators.size() - 1}), attribute_mode::keep);
}

expr_id builder::lambda_expression(form_id form, run_parts const& parts)
{
    auto const left = parts.operands[0];
    auto const right = parts.operands[1];
    if (is_keyword_led(left))
    {
        // `=>` is looser than a keyword form, so `yield x => x` arrives as `(yield x) => x`, and
        // `return fun (x) => x` as `(return fun (x)) => x`: the jumps take the lambda as their value.
        auto const left_parts = keyword_parts_of(left);
        auto jump_count = isize(0);
        while (jump_count < left_parts.keywords.size() && is_value_jump(token_text_of(left_parts.keywords[jump_count])))
            ++jump_count;

        auto result = expr_id::none;
        auto const is_arrow_lambda = jump_count == left_parts.keywords.size() && left_parts.arguments.size() == 1
                                  && !is_valid(left_parts.block);
        auto const is_fun_lambda = jump_count + 1 == left_parts.keywords.size()
                                && token_text_of(left_parts.keywords[jump_count]) == "fun"
                                && is_anonymous_fun(left_parts);
        if (is_arrow_lambda)
            result = arrow_lambda_expression(form, left_parts.arguments[0], right);
        else if (is_fun_lambda)
            result = fun_lambda_expression(form, left, left_parts, right);
        else
            return invalid_expression(form, diagnostic_kind::statement_in_expression);

        for (auto i = jump_count - 1; i >= 0; --i)
            result = make_jump(form, token_text_of(left_parts.keywords[i]), result);
        return result;
    }
    return arrow_lambda_expression(form, left, right);
}

expr_id builder::arrow_lambda_expression(form_id form, form_id left, form_id right)
{
    auto parameters = range_of<field>();
    if (is_kind(left, form_kind::round_list))
        parameters = fields_of(left, diagnostic_kind::expected_parameter);
    else if (is_kind(left, form_kind::identifier) || is_kind(left, form_kind::wildcard))
        parameters = append_one(ast.fields, field{.form = left, .name = at(left).where});
    else
        return invalid_expression(form, diagnostic_kind::expected_parameter);

    auto const result = value_body(right, body_owner::arrow_lambda);
    return make_expr(form, lambda{.parameters = parameters, .body = result});
}

expr_id builder::fun_lambda_expression(form_id form, form_id keyword_form, keyword_parts const& parts, form_id right_of_arrow)
{
    auto const signature = signature_of(parts);
    if (!signature.has_parameter_list)
        report(diagnostic_kind::missing_parameter_list, keyword_form);

    auto result = lambda{.spelling = lambda_spelling::fun,
                         .type_parameters = signature.type_parameters,
                         .parameters = signature.parameters,
                         .bindings = signature.bindings,
                         .return_type = signature.return_type};
    if (is_valid(right_of_arrow))
    {
        if (is_valid(parts.block))
            report(diagnostic_kind::too_many_arguments, parts.block);
        result.body = value_body(right_of_arrow, body_owner::function);
    }
    else if (is_valid(parts.block))
        result.body = value_body(parts.block, body_owner::function);
    else
        report(diagnostic_kind::expected_body, keyword_form);
    return make_expr(form, result);
}

expr_id builder::keyword_expression(form_id form)
{
    if (!is_keyword_led(form))
        return invalid_expression(form, diagnostic_kind::unsupported_syntax);
    return keyword_expression_from(form, keyword_parts_of(form), 0);
}

expr_id builder::keyword_expression_from(form_id form, keyword_parts const& parts, isize first_keyword)
{
    auto const keyword = token_text_of(parts.keywords[first_keyword]);
    auto const has_more = first_keyword + 1 < parts.keywords.size();

    // Consecutive keywords head ONE keyword form, so `return case x:` arrives as a form with two keywords.
    // A jump takes the rest as its value, and the arguments and the block are the inner expression's.
    if (is_value_jump(keyword) && has_more)
    {
        auto const value = keyword_expression_from(form, parts, first_keyword + 1);
        return make_jump(form, keyword, value);
    }
    if (has_more)
        return invalid_expression(form, keyword == "let" || keyword == "else" ? diagnostic_kind::statement_in_expression
                                                                              : diagnostic_kind::unexpected_keyword);

    if (keyword == "case")
        return case_expression(form, parts);
    if (is_value_jump(keyword) || keyword == "continue")
        return jump_expression(form, parts, keyword);
    if (keyword == "fun" && is_anonymous_fun(parts))
        return fun_lambda_expression(form, form, parts, form_id::none);
    if (keyword == "loop")
    {
        if (!parts.arguments.empty())
            report(diagnostic_kind::too_many_arguments, parts.arguments[0]);
        if (!is_valid(parts.block))
            report(diagnostic_kind::expected_body, form);
        owners.push_back({.owner = body_owner::value_loop});
        auto const result = is_valid(parts.block) ? block_body(parts.block) : body();
        owners.remove_back();
        return make_expr(form, loop_expr{.body = result});
    }
    return invalid_expression(
        form, keyword == "mut" ? diagnostic_kind::unexpected_keyword : diagnostic_kind::statement_in_expression);
}

expr_id builder::case_expression(form_id form, keyword_parts const& parts)
{
    auto value = expr_id::none;
    if (parts.arguments.empty())
        value = invalid_expression(form, diagnostic_kind::expected_expression);
    else
        value = expression(parts.arguments[0]);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);
    if (!is_valid(parts.block))
    {
        report(diagnostic_kind::expected_body, form);
        return make_expr(form, case_expr{.value = value});
    }

    auto collected = cc::vector<case_arm>();
    for (auto line = at(parts.block).first_child; is_valid(line); line = at(line).next_sibling)
    {
        reject_attributes(line);
        auto const arm_parts = is_kind(line, form_kind::operator_run) ? run_parts_of(line) : run_parts();
        auto const is_arm = arm_parts.operands.size() == 2 && arm_parts.operators.size() == 1
                         && level_of(arm_parts.operators[0]) == operator_level::computes_as
                         && !is_keyword_led(arm_parts.operands[0]);
        if (!is_arm)
        {
            auto const result = invalid_expression(line, diagnostic_kind::expected_case_arm);
            collected.push_back({.form = line, .result = {.kind = body_kind::arrow, .form = line, .value = result}});
            continue;
        }
        auto const pattern = expression(arm_parts.operands[0]);
        auto const result = value_body(arm_parts.operands[1], body_owner::value_block);
        collected.push_back({.form = line, .pattern = pattern, .result = result});
    }
    auto const arms = append(ast.case_arms, cc::span<case_arm const>(collected));
    return make_expr(form, case_expr{.value = value, .arms = arms});
}

expr_id builder::jump_expression(form_id form, keyword_parts const& parts, cc::string_view keyword)
{
    auto const takes_value = keyword != "continue";
    auto const allowed = takes_value ? isize(1) : isize(0);
    if (parts.arguments.size() > allowed)
        report(diagnostic_kind::too_many_arguments, parts.arguments[allowed]);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);

    auto value = takes_value && !parts.arguments.empty() ? expression(parts.arguments[0]) : expr_id::none;
    if (!takes_value)
        return make_jump(form, keyword, expr_id::none);
    // A `yield` without a value hands nothing on, which is what leaving the block out would have said.
    if (keyword == "yield" && !is_valid(value))
        value = invalid_expression(form, diagnostic_kind::expected_expression);
    return make_jump(form, keyword, value);
}

expr_id builder::make_jump(form_id form, cc::string_view keyword, expr_id value)
{
    report_jump_target(form, keyword);
    if (keyword == "continue")
        return make_expr(form, continue_expr{});
    if (keyword == "break")
        return make_expr(form, break_expr{.value = value});
    if (keyword == "yield")
        return make_expr(form, yield_expr{.value = value});
    return make_expr(form, return_expr{.value = value});
}

void builder::report_jump_target(form_id form, cc::string_view keyword)
{
    auto const is_loop_jump = keyword == "break" || keyword == "continue";
    auto crosses_value_loop = false;
    for (auto i = owners.size() - 1; i >= 0; --i)
    {
        auto const owner = owners[i].owner;
        auto const is_loop = owner == body_owner::value_loop || owner == body_owner::statement_loop;
        auto const is_one_line = owners[i].one_line == form;

        if (is_loop_jump)
        {
            if (is_loop)
                return;
            // A `case` arm is looked through: `_ => break` leaves the loop around the `case`.
            if (owner == body_owner::value_block)
                continue;
            break;
        }
        if (is_loop)
        {
            crosses_value_loop |= owner == body_owner::value_loop;
            continue;
        }
        if (keyword == "yield")
        {
            if (owner == body_owner::function)
                report(diagnostic_kind::yield_in_function, form);
            else if (crosses_value_loop)
                report(diagnostic_kind::yield_in_loop, form);
            else if (is_one_line)
                report(diagnostic_kind::redundant_yield, form);
            return;
        }

        // A `case` arm and a property are looked through: `_ => return false` leaves the function around the `case`.
        if (owner == body_owner::value_block)
            continue;
        if (owner == body_owner::arrow_lambda)
            report(is_one_line ? diagnostic_kind::redundant_return : diagnostic_kind::return_in_lambda, form);
        return;
    }
    report(diagnostic_kind::jump_without_target, form);
}
