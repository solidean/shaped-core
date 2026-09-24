#include <shaped-graphics-language/ast/impl/builder.hh>

using namespace sgl;
using namespace sgl::ast;
using namespace sgl::ast::impl;

bool builder::is_declaration_keyword(cc::string_view keyword)
{
    return keyword == "module" || keyword == "use" || keyword == "fun" || keyword == "struct" || keyword == "enum"
        || keyword == "type" || keyword == "const" || keyword == "binding" || keyword == "sampler"
        || keyword == "pipeline" || keyword == "notation";
}

range_of<decl_id> builder::declarations(form_id block, scope_kind scope)
{
    auto const lines = lines_of(block);
    auto collected = cc::vector<decl_id>();
    for (auto i = isize(0); i < lines.size(); ++i)
    {
        auto const head = head_of(lines[i]);
        if (is_valid(head.keyword_form) || scope == scope_kind::file)
            collected.push_back(declaration(head, scope, scope == scope_kind::file && i == 0));
        else
            collected.push_back(member_declaration(lines[i], scope));
    }
    return append(ast.decl_lists, cc::span<decl_id const>(collected));
}

decl_id builder::declaration(statement_head const& head, scope_kind scope, bool is_first_in_file)
{
    auto const is_member_scope = scope != scope_kind::file && scope != scope_kind::function;
    auto const not_a_declaration
        = is_member_scope ? diagnostic_kind::expected_member : diagnostic_kind::expected_declaration;
    if (!is_valid(head.keyword_form))
        return invalid_declaration(head.whole, not_a_declaration);

    auto const parts = keyword_parts_of(head.keyword_form);
    auto const keyword = token_text_of(parts.keywords[0]);
    if (keyword == "let")
        return invalid_declaration(head.whole, diagnostic_kind::declaration_not_allowed_here);
    if (!is_declaration_keyword(keyword))
        return invalid_declaration(head.whole,
                                   keyword == "mut" ? diagnostic_kind::unexpected_keyword : not_a_declaration);
    if (parts.keywords.size() > 1)
    {
        report(diagnostic_kind::unexpected_keyword, parts.keywords[1]);
        return make_decl(head.whole, attributes_of(head.whole), invalid_decl{});
    }

    // A misplaced declaration is still read: where it stands is wrong, what it says is not.
    if (keyword == "module" && !is_first_in_file)
        report(diagnostic_kind::misplaced_module, head.keyword_form);
    else if (keyword == "sampler")
    {
        // AST-132: a static sampler stands at file scope, or in a binding, whose group layout it then belongs to.
        if (scope != scope_kind::file && scope != scope_kind::binding_body)
            report(diagnostic_kind::declaration_not_allowed_here, head.keyword_form);
    }
    else if (keyword == "pipeline" && scope != scope_kind::file)
        report(diagnostic_kind::declaration_not_allowed_here, head.keyword_form);
    else if (scope == scope_kind::binding_body)
        report(diagnostic_kind::member_not_allowed_here, head.keyword_form);

    if (keyword == "module")
        return module_declaration(head, parts);
    if (keyword == "use")
        return use_declaration(head, parts);
    if (keyword == "fun")
        return fun_declaration(head, parts);
    if (keyword == "struct")
        return type_body_declaration(head, parts, scope_kind::struct_body);
    if (keyword == "enum")
        return type_body_declaration(head, parts, scope_kind::enum_body);
    if (keyword == "binding")
        return type_body_declaration(head, parts, scope_kind::binding_body);
    if (keyword == "type")
        return type_declaration(head, parts);
    if (keyword == "const")
        return const_declaration(head, parts);
    if (keyword == "sampler")
        return sampler_declaration(head, parts);
    if (keyword == "pipeline")
        return pipeline_declaration(head, parts);
    return notation_declaration(head, parts);
}

decl_id builder::member_declaration(form_id line, scope_kind owner)
{
    if (is_kind(line, form_kind::identifier))
    {
        if (owner != scope_kind::enum_body)
            report(diagnostic_kind::member_not_allowed_here, line);
        return make_decl(line, attributes_of(line), enum_case_decl{.name = at(line).where});
    }

    auto const is_valued_case = owner == scope_kind::enum_body && is_binary_run(line, "=")
                             && is_kind(at(line).first_child, form_kind::identifier);
    if (is_valued_case)
    {
        auto const attributes = attributes_of(line);
        auto const case_parts = run_parts_of(line);
        auto const value = expression(case_parts.operands[1]);
        return make_decl(line, attributes, enum_case_decl{.name = at(case_parts.operands[0]).where, .value = value});
    }

    auto const line_parts = is_kind(line, form_kind::operator_run) ? run_parts_of(line) : run_parts();
    auto const is_property = line_parts.operands.size() == 2 && line_parts.operators.size() == 1
                          && level_of(line_parts.operators[0]) == operator_level::computes_as
                          && is_kind(line_parts.operands[0], form_kind::identifier);
    if (is_property)
    {
        auto const attributes = attributes_of(line);
        auto const value = value_body(line_parts.operands[1], body_owner::value_block);
        return make_decl(line, attributes, property_decl{.name = at(line_parts.operands[0]).where, .body = value});
    }

    if (is_field_like(line, true))
    {
        if (owner == scope_kind::enum_body)
            report(diagnostic_kind::member_not_allowed_here, line);
        auto const made = make_field(line, diagnostic_kind::expected_member);
        if (owner == scope_kind::binding_body && is_valid(made.default_value))
            report(diagnostic_kind::default_not_allowed_here, ast.at(made.default_value).form);
        ast.fields.push_back(made);
        return make_decl(line, {}, field_decl{.field = field_id(i32(ast.fields.size() - 1))});
    }
    return invalid_declaration(line, diagnostic_kind::expected_member);
}

source_span builder::declared_name(form_id keyword_form, keyword_parts const& parts)
{
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);
    if (!parts.arguments.empty() && is_kind(parts.arguments[0], form_kind::identifier))
        return at(parts.arguments[0]).where;
    report(diagnostic_kind::expected_name, parts.arguments.empty() ? keyword_form : parts.arguments[0]);
    return {};
}

void builder::reject_arrow(statement_head const& head)
{
    if (is_valid(head.arrow))
        report(diagnostic_kind::unexpected_token, head.arrow_operator);
}

void builder::reject_assignment(statement_head const& head)
{
    if (is_valid(head.assign_value))
        report(diagnostic_kind::unexpected_token, head.assign_operator);
}

decl_id builder::module_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    reject_assignment(head);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    auto result = module_decl();
    auto const path = parts.arguments.empty() ? form_id::none : parts.arguments[0];
    if (is_kind(path, form_kind::identifier) || is_kind(path, form_kind::member))
        result.path = expression(path);
    else
        result.path = invalid_expression(is_valid(path) ? path : head.keyword_form, diagnostic_kind::expected_name);
    return make_decl(head.whole, attributes, result);
}

decl_id builder::use_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    reject_assignment(head);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    auto result = use_decl();
    auto path = parts.arguments.empty() ? form_id::none : parts.arguments[0];
    if (is_binary_run(path, "as"))
    {
        auto const path_parts = run_parts_of(path);
        if (is_kind(path_parts.operands[1], form_kind::identifier))
            result.alias = at(path_parts.operands[1]).where;
        else
            report(diagnostic_kind::expected_name, path_parts.operands[1]);
        path = path_parts.operands[0];
    }
    if (is_kind(path, form_kind::identifier) || is_kind(path, form_kind::member))
        result.path = expression(path);
    else
        result.path = invalid_expression(is_valid(path) ? path : head.keyword_form, diagnostic_kind::expected_name);
    return make_decl(head.whole, attributes, result);
}

bool builder::is_anonymous_fun(keyword_parts const& parts) const
{
    auto base = parts.arguments.empty() ? form_id::none : parts.arguments[0];
    if (is_kind(base, form_kind::operator_run) && is_valid(at(base).first_child))
        base = at(base).first_child;
    while (is_kind(base, form_kind::call))
        base = at(base).first_child;
    return !is_valid(base) || is_kind(base, form_kind::round_list) || is_kind(base, form_kind::square_list)
        || is_kind(base, form_kind::curly_list);
}

fun_signature builder::signature_of(keyword_parts const& parts)
{
    auto result = fun_signature();
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    auto signature = parts.arguments.empty() ? form_id::none : parts.arguments[0];
    auto const signature_parts = is_kind(signature, form_kind::operator_run) ? run_parts_of(signature) : run_parts();
    auto const marker_level
        = signature_parts.operators.empty() ? operator_level::arithmetic : level_of(signature_parts.operators[0]);
    if (marker_level == operator_level::arrow || marker_level == operator_level::ascription)
    {
        // Only `->` introduces a return type; after any other marker the type is still read as one.
        if (marker_level != operator_level::arrow)
            report(diagnostic_kind::unexpected_token, signature_parts.operators[0]);
        result.return_type = type_after_first_operator(signature, signature_parts);
        signature = signature_parts.operands[0];
    }

    // The lists are fused to the name, so the signature is a call of a call; the outermost list was written last.
    // Without a name the first list stands where the name would, and the rest are fused to it.
    auto lists = cc::vector<form_id>();
    auto name_form = signature;
    while (is_kind(name_form, form_kind::call))
    {
        lists.push_back(at(at(name_form).first_child).next_sibling);
        name_form = at(name_form).first_child;
    }
    auto const is_list = is_kind(name_form, form_kind::round_list) || is_kind(name_form, form_kind::square_list)
                      || is_kind(name_form, form_kind::curly_list);
    if (is_list)
    {
        lists.push_back(name_form);
        result.first_list = name_form;
    }
    else
        result.name_form = name_form;

    // `[type parameters]`, `(parameters)`, `{bindings}`: each at most once, in this order.
    auto const rank_of = [&](form_id list)
    {
        return at(list).kind == form_kind::square_list ? 0 : at(list).kind == form_kind::round_list ? 1 : 2;
    };
    bool seen[3] = {false, false, false};
    auto highest = -1;
    for (auto i = lists.size() - 1; i >= 0; --i)
    {
        auto const list = lists[i];
        auto const rank = rank_of(list);
        if (seen[rank])
        {
            report(diagnostic_kind::duplicate_signature_list, list);
            continue;
        }
        if (rank < highest)
            report(diagnostic_kind::signature_out_of_order, list);
        seen[rank] = true;
        highest = rank > highest ? rank : highest;

        if (rank == 0)
            result.type_parameters = fields_of(list, diagnostic_kind::expected_parameter);
        else if (rank == 1)
            result.parameters = fields_of(list, diagnostic_kind::expected_parameter);
        else
            result.bindings = list_elements(list, false, true);
    }
    result.has_parameter_list = seen[1];
    return result;
}

decl_id builder::fun_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    auto const signature = signature_of(parts);
    auto result = fun_decl{.type_parameters = signature.type_parameters,
                           .parameters = signature.parameters,
                           .bindings = signature.bindings,
                           .return_type = signature.return_type};

    if (!is_kind(signature.name_form, form_kind::identifier))
    {
        auto const nameless = is_valid(signature.name_form) ? signature.name_form : signature.first_list;
        report(diagnostic_kind::expected_name, is_valid(nameless) ? nameless : head.keyword_form);
    }
    else
    {
        result.name = at(signature.name_form).where;
        if (!signature.has_parameter_list)
            report(diagnostic_kind::missing_parameter_list, signature.name_form);
    }

    auto const parameters = ast.at(result.parameters);
    if (!parameters.empty() && file.text_of(parameters[0].name) == "self" && !is_valid(parameters[0].type))
        result.receiver = parameters[0].is_mut ? receiver_kind::mut_self : receiver_kind::self;

    if (is_valid(head.assign_value))
    {
        // `fun f() = x`, and `fun f() => x = 1`, whose value would be an assignment.
        report(diagnostic_kind::expected_body, head.assign_operator);
        (void)expression(head.assign_value);
    }
    if (is_valid(head.arrow))
    {
        if (is_valid(parts.block))
            report(diagnostic_kind::too_many_arguments, parts.block);
        result.body = value_body(head.arrow, body_owner::function);
    }
    else if (is_valid(parts.block))
        result.body = value_body(parts.block, body_owner::function);
    return make_decl(head.whole, attributes, result);
}

decl_id builder::type_body_declaration(statement_head const& head, keyword_parts const& parts, scope_kind body_scope)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    auto const declared = declared_name(head.keyword_form, parts);
    auto const members = is_valid(parts.block) ? declarations(parts.block, body_scope) : range_of<decl_id>();

    if (body_scope == scope_kind::binding_body)
    {
        auto result = binding_decl{.name = declared, .members = members};
        if (is_valid(head.assign_value))
        {
            if (token_text_of(head.assign_operator) != "=")
                report(diagnostic_kind::unexpected_token, head.assign_operator);
            result.composition = expression(head.assign_value);
        }
        return make_decl(head.whole, attributes, result);
    }

    reject_assignment(head);
    if (body_scope == scope_kind::struct_body)
        return make_decl(head.whole, attributes,
                         struct_decl{.name = declared, .members = members, .is_opaque = !is_valid(parts.block)});
    return make_decl(head.whole, attributes, enum_decl{.name = declared, .members = members});
}

decl_id builder::type_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);

    auto result = type_decl{.name = declared_name(head.keyword_form, parts)};
    if (is_valid(head.assign_value) && token_text_of(head.assign_operator) == "=")
        result.value = type_expression(head.assign_value);
    else
        result.value = invalid_expression(head.whole, diagnostic_kind::expected_expression);
    return make_decl(head.whole, attributes, result);
}

decl_id builder::const_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    auto result = const_decl();
    auto target = parts.arguments.empty() ? form_id::none : parts.arguments[0];
    auto const target_parts = is_kind(target, form_kind::operator_run) ? run_parts_of(target) : run_parts();
    if (!target_parts.operators.empty() && token_text_of(target_parts.operators[0]) == ":")
    {
        result.type = type_after_first_operator(target, target_parts);
        target = target_parts.operands[0];
    }
    if (is_kind(target, form_kind::identifier))
        result.name = at(target).where;
    else
        report(diagnostic_kind::expected_name, is_valid(target) ? target : head.keyword_form);

    if (is_valid(head.assign_value) && token_text_of(head.assign_operator) == "=")
        result.value = expression(head.assign_value);
    else
        result.value = invalid_expression(head.whole, diagnostic_kind::expected_expression);
    return make_decl(head.whole, attributes, result);
}

decl_id builder::sampler_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    reject_assignment(head);
    auto result = sampler_decl{.name = declared_name(head.keyword_form, parts)};

    auto collected = cc::vector<argument>();
    if (is_valid(parts.block))
    {
        for (auto const line : lines_of(parts.block))
        {
            auto const is_setting = is_binary_run(line, "=") && is_kind(at(line).first_child, form_kind::identifier);
            if (is_setting)
                collected.push_back(list_element(line, false, false));
            else
                collected.push_back({.form = line, .value = invalid_expression(line, diagnostic_kind::expected_member)});
        }
    }
    result.settings = append(ast.arguments, cc::span<argument const>(collected));
    return make_decl(head.whole, attributes, result);
}

decl_id builder::pipeline_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_arrow(head);
    auto result = pipeline_decl();

    // The name is optional: `pipeline:` declares the file's pipeline, which a later phase names.
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);
    if (!parts.arguments.empty())
    {
        if (is_kind(parts.arguments[0], form_kind::identifier))
            result.name = at(parts.arguments[0]).where;
        else
            report(diagnostic_kind::expected_name, parts.arguments[0]);
    }

    auto settings_block = parts.block;
    if (is_valid(head.assign_value))
    {
        if (token_text_of(head.assign_operator) != "=")
            report(diagnostic_kind::unexpected_token, head.assign_operator);
        if (is_valid(parts.block))
            report(diagnostic_kind::too_many_arguments, parts.block);
        result.is_short_form = true;

        // `pipeline = (a, b):` hangs its settings block off the list, the rightmost form of the line (FORM-34).
        auto stages = head.assign_value;
        settings_block = form_id::none;
        if (is_kind(stages, form_kind::keyword_form))
        {
            auto const inner = keyword_parts_of(stages);
            if (inner.keywords.empty() && inner.arguments.size() == 1 && is_valid(inner.block))
            {
                stages = inner.arguments[0];
                settings_block = inner.block;
            }
        }
        if (is_kind(stages, form_kind::round_list))
            result.stages = list_elements(stages, false, false);
        else
            report(diagnostic_kind::expected_expression, stages);
    }

    auto collected = cc::vector<setting>();
    if (is_valid(settings_block))
    {
        for (auto const line : lines_of(settings_block))
        {
            if (!is_binary_run(line, "="))
            {
                collected.push_back({.form = line, .value = invalid_expression(line, diagnostic_kind::expected_member)});
                continue;
            }
            auto const line_parts = run_parts_of(line);
            auto const target = line_parts.operands[0];
            auto const is_path = is_kind(target, form_kind::identifier) || is_kind(target, form_kind::member);
            collected.push_back(
                {.form = line,
                 .path = is_path ? expression(target) : invalid_expression(target, diagnostic_kind::expected_name),
                 .value = expression(line_parts.operands[1])});
        }
    }
    result.settings = append(ast.settings, cc::span<setting const>(collected));
    return make_decl(head.whole, attributes, result);
}

decl_id builder::notation_declaration(statement_head const& head, keyword_parts const& parts)
{
    auto const attributes = attributes_of(head.whole);
    reject_assignment(head);
    if (is_valid(parts.block))
        report(diagnostic_kind::too_many_arguments, parts.block);
    if (parts.arguments.size() > 1)
        report(diagnostic_kind::too_many_arguments, parts.arguments[1]);

    auto result = notation_decl();
    if (parts.arguments.empty())
        result.pattern = invalid_expression(head.keyword_form, diagnostic_kind::expected_expression);
    else
        result.pattern = expression(parts.arguments[0]);
    if (is_valid(head.arrow))
        result.replacement = expression(head.arrow);
    else
        result.replacement = invalid_expression(head.whole, diagnostic_kind::expected_expression);
    return make_decl(head.whole, attributes, result);
}
