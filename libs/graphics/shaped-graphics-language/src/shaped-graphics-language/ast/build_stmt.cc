#include <shaped-graphics-language/ast/impl/builder.hh>

using namespace sgl;
using namespace sgl::ast;
using namespace sgl::ast::impl;

range_of<stmt_id> builder::statements(form_id block)
{
    auto const lines = lines_of(block);
    auto const starts_with
        = [&](form_id line, cc::string_view keyword) { return is_keyword_led(head_of(line).keyword_form, keyword); };

    auto collected = cc::vector<stmt_id>();
    auto i = isize(0);
    while (i < lines.size())
    {
        if (!starts_with(lines[i], "if") && !starts_with(lines[i], "else"))
        {
            collected.push_back(statement(head_of(lines[i])));
            ++i;
            continue;
        }

        // An `else` pairs with the sibling directly above it, and a plain `else` closes the chain.
        auto end = i + 1;
        auto is_closed
            = starts_with(lines[i], "else") && keyword_parts_of(head_of(lines[i]).keyword_form).keywords.size() == 1;
        while (!is_closed && end < lines.size() && starts_with(lines[end], "else"))
        {
            is_closed = keyword_parts_of(head_of(lines[end]).keyword_form).keywords.size() == 1;
            ++end;
        }
        collected.push_back(if_chain(cc::span<form_id const>(lines).subspan({.offset = i, .size = end - i})));
        i = end;
    }
    return append(ast.stmt_lists, cc::span<stmt_id const>(collected));
}

stmt_id builder::statement(statement_head const& head)
{
    auto const form = head.whole;
    if (!is_valid(head.keyword_form))
    {
        if (is_kind(form, form_kind::operator_run))
        {
            auto const parts = run_parts_of(form);
            auto const is_assignment = parts.operands.size() == 2 && parts.operators.size() == 1
                                    && level_of(parts.operators[0]) == operator_level::assignment;
            if (is_assignment)
                return assignment(form, parts.operands[0], parts.operators[0], parts.operands[1], true);
        }
        return expression_statement(form);
    }

    auto const parts = keyword_parts_of(head.keyword_form);
    auto const keyword = token_text_of(parts.keywords[0]);
    auto const second = parts.keywords.size() > 1 ? token_text_of(parts.keywords[1]) : cc::string_view();

    if (keyword == "if" || keyword == "else")
    {
        form_id const chain[] = {form};
        return if_chain(cc::span<form_id const>(chain));
    }
    if (keyword == "let" && (second.empty() || (second == "mut" && parts.keywords.size() == 2)))
        return let_statement(head, parts);
    if (parts.keywords.size() > 1 && !is_value_jump(keyword))
    {
        report(diagnostic_kind::unexpected_keyword, parts.keywords[1]);
        return make_stmt(form, attributes_of(form), invalid_stmt{});
    }

    if (keyword == "for")
        return for_statement(head, parts);
    if (keyword == "while")
        return while_statement(head, parts);
    if (keyword == "assert")
        return assert_statement(head, parts);
    if (keyword == "print")
        return print_statement(head, parts);
    if (is_declaration_keyword(keyword))
    {
        auto const declaration_id = declaration(head, scope_kind::function, false);
        return make_stmt(form, {}, decl_stmt{.declaration = declaration_id});
    }
    if (keyword == "mut")
    {
        report(diagnostic_kind::unexpected_keyword, parts.keywords[0]);
        return make_stmt(form, attributes_of(form), invalid_stmt{});
    }
    // `case`, `loop` and the jumps are expressions.
    return expression_statement(form);
}

stmt_id builder::assignment(form_id whole, form_id target, form_id op, form_id value, bool takes_attributes)
{
    auto const attributes = takes_attributes ? attributes_of(whole) : range_of<attribute>();
    auto const target_id = expression(target);
    auto const value_id = expression(value);
    return make_stmt(whole, attributes, assign_stmt{.target = target_id, .op = at(op).token, .value = value_id});
}

stmt_id builder::expression_statement(form_id form)
{
    auto const attributes = attributes_of(form);
    auto const value = expression(form, attribute_mode::taken);

    auto const& node = ast.at(value).node;
    auto const* as_call = node.try_as<call>();
    auto const is_application
        = as_call != nullptr
       && (as_call->spelling == call_spelling::paren || as_call->spelling == call_spelling::juxtaposition);
    auto const has_effect = is_application || node.is<return_expr>() || node.is<yield_expr>() || node.is<break_expr>()
                         || node.is<continue_expr>() || node.is<case_expr>() || node.is<loop_expr>()
                         || node.is<with_bindings>() || node.is<invalid_expr>();
    if (!has_effect)
        report(diagnostic_kind::no_effect, form);
    return make_stmt(form, attributes, expr_stmt{.value = value});
}

stmt_id builder::let_statement(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto result = let_stmt{.is_mut = parts.keywords.size() == 2};
    if (is_valid(head.arrow))
        report(diagnostic_kind::unexpected_token, head.arrow_operator);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);

    if (parts.arguments.empty())
        result.pattern = invalid_expression(head.keyword_form, diagnostic_kind::expected_pattern);
    else
    {
        auto target = parts.arguments[0];
        auto type_run = form_id::none;
        auto const target_parts = is_kind(target, form_kind::operator_run) ? run_parts_of(target) : run_parts();
        if (!target_parts.operators.empty() && token_text_of(target_parts.operators[0]) == ":")
        {
            type_run = target;
            target = target_parts.operands[0];
        }

        // A pattern is a name, `_`, or a round list of patterns.
        auto const is_pattern = [&](auto const& self, form_id f) -> bool
        {
            if (is_kind(f, form_kind::identifier) || is_kind(f, form_kind::wildcard))
                return true;
            if (!is_kind(f, form_kind::round_list))
                return false;
            for (auto e = at(f).first_child; is_valid(e); e = at(e).next_sibling)
                if (!self(self, e))
                    return false;
            return true;
        };
        result.pattern = is_pattern(is_pattern, target) ? expression(target)
                                                        : invalid_expression(target, diagnostic_kind::expected_pattern);
        if (is_valid(type_run))
            result.type = type_after_first_operator(type_run, target_parts);
        if (parts.arguments.size() > 1)
            report(diagnostic_kind::too_many_arguments, parts.arguments[1]);
    }

    if (is_valid(head.assign_value))
    {
        if (token_text_of(head.assign_operator) != "=")
            report(diagnostic_kind::unexpected_token, head.assign_operator);
        result.value = expression(head.assign_value);
    }
    return make_stmt(head.whole, attributes, result);
}

stmt_id builder::for_statement(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto result = for_stmt();

    auto const header = parts.arguments.size() == 1 ? parts.arguments[0] : form_id::none;
    auto const header_parts = is_kind(header, form_kind::operator_run) ? run_parts_of(header) : run_parts();
    auto const is_name_in_range = header_parts.operands.size() == 2 && header_parts.operators.size() == 1
                               && token_text_of(header_parts.operators[0]) == "in"
                               && (is_kind(header_parts.operands[0], form_kind::identifier)
                                   || is_kind(header_parts.operands[0], form_kind::wildcard));
    if (is_name_in_range)
    {
        result.variable = expression(header_parts.operands[0]);
        result.iterable = expression(header_parts.operands[1]);
    }
    else
    {
        report(diagnostic_kind::for_takes_name_in_range, head.keyword_form);
        // Whatever was written is still read, so nothing of it is lost and its own errors are found.
        for (auto const argument_form : parts.arguments)
            result.iterable = expression(argument_form);
    }

    result.body = statement_body(head, parts);
    return make_stmt(head.whole, attributes, result);
}

stmt_id builder::while_statement(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto result = while_stmt();
    if (parts.arguments.empty())
        result.condition = invalid_expression(head.keyword_form, diagnostic_kind::expected_expression);
    else
        result.condition = expression(parts.arguments[0]);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    result.body = statement_body(head, parts);
    return make_stmt(head.whole, attributes, result);
}

stmt_id builder::assert_statement(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto const is_well_formed = (parts.arguments.size() == 1 || parts.arguments.size() == 2) && !is_valid(parts.block)
                             && !is_valid(head.arrow) && !is_valid(head.assign_value);
    if (!is_well_formed)
        report(diagnostic_kind::assert_takes_condition_and_message, head.whole);

    auto result = assert_stmt();
    if (!parts.arguments.empty())
        result.condition = expression(parts.arguments[0]);
    if (parts.arguments.size() > 1)
        result.message = expression(parts.arguments[1]);
    return make_stmt(head.whole, attributes, result);
}

stmt_id builder::print_statement(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto const is_well_formed = parts.arguments.size() == 1 && !is_valid(parts.block) && !is_valid(head.arrow)
                             && !is_valid(head.assign_value);
    if (!is_well_formed)
        report(diagnostic_kind::print_takes_one_message, head.whole);

    auto result = print_stmt();
    if (!parts.arguments.empty())
        result.message = expression(parts.arguments[0]);
    return make_stmt(head.whole, attributes, result);
}

stmt_id builder::if_chain(cc::span<form_id const> chain)
{
    auto const attributes = attributes_of(chain[0]);

    auto collected = cc::vector<if_branch>();
    for (auto i = isize(0); i < chain.size(); ++i)
    {
        auto const head = head_of(chain[i]);
        auto const parts = keyword_parts_of(head.keyword_form);
        auto const is_else = token_text_of(parts.keywords[0]) == "else";
        auto const has_condition = !is_else || parts.keywords.size() > 1;

        if (i > 0)
            reject_attributes(chain[i]);
        else if (is_else)
            report(diagnostic_kind::stray_else, parts.keywords[0]);

        auto const is_known = parts.keywords.size() == 1
                           || (is_else && parts.keywords.size() == 2 && token_text_of(parts.keywords[1]) == "if");
        if (!is_known)
            report(diagnostic_kind::unexpected_keyword, parts.keywords[1]);

        auto branch = if_branch{.form = chain[i]};
        auto const allowed = has_condition ? isize(1) : isize(0);
        if (has_condition && parts.arguments.empty())
            branch.condition = invalid_expression(head.keyword_form, diagnostic_kind::expected_expression);
        else if (has_condition)
            branch.condition = expression(parts.arguments[0]);
        if (parts.arguments.size() > allowed)
            report(diagnostic_kind::too_many_arguments, parts.arguments[allowed]);

        branch.then = statement_body(head, parts);
        collected.push_back(branch);
    }

    auto const branches = append(ast.if_branches, cc::span<if_branch const>(collected));
    return make_stmt(chain[0], attributes, if_stmt{.branches = branches});
}

body builder::block_body(form_id block)
{
    auto const list = statements(block);
    return {.kind = body_kind::block, .form = block, .statements = list};
}

body builder::value_body(form_id right_of_arrow, body_owner owner)
{
    owners.push_back(owner);
    auto result = body();
    if (is_kind(right_of_arrow, form_kind::block))
        result = block_body(right_of_arrow);
    else
    {
        auto const value = expression(right_of_arrow);
        result = {.kind = body_kind::arrow, .form = right_of_arrow, .value = value};
    }
    owners.remove_back();
    return result;
}

body builder::statement_body(statement_head const& head, keyword_parts const& parts)
{
    if (is_valid(head.arrow))
    {
        if (is_valid(parts.block))
            report(diagnostic_kind::too_many_arguments, parts.block);
        if (is_kind(head.arrow, form_kind::block))
            return block_body(head.arrow);

        // `if done => total = 0`: the assignment is the statement right of the arrow.
        auto const only = is_valid(head.assign_value)
                            ? assignment(head.whole, head.arrow, head.assign_operator, head.assign_value, false)
                            : statement(head_of(head.arrow));
        auto const list = append_one(ast.stmt_lists, only);
        return {.kind = body_kind::arrow, .form = head.arrow, .statements = list};
    }

    if (is_valid(head.assign_value))
        report(diagnostic_kind::unexpected_token, head.assign_operator);
    if (is_valid(parts.block))
        return block_body(parts.block);
    report(diagnostic_kind::expected_body, head.keyword_form);
    return {};
}
