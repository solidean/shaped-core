#include "build.hh"

#include <shaped-graphics-language/ast/impl/builder.hh>

using namespace sgl;
using namespace sgl::ast;
using namespace sgl::ast::impl;

namespace
{
bool is_comparison(cc::string_view s)
{
    return s == "<" || s == "<=" || s == "==" || s == "!=" || s == ">=" || s == ">";
}
} // namespace

// ---- forms ----------------------------------------------------------------------------------------------------

cc::string_view builder::token_text_of(form_id id) const
{
    auto const token = at(id).token;
    return is_valid(token) ? file.text_of(file.at(token).where) : cc::string_view();
}

keyword_parts builder::keyword_parts_of(form_id keyword_form) const
{
    auto result = keyword_parts();
    for (auto child = at(keyword_form).first_child; is_valid(child); child = at(child).next_sibling)
    {
        if (at(child).kind == form_kind::keyword)
            result.keywords.push_back(child);
        else if (at(child).kind == form_kind::block)
            result.block = child;
        else
            result.arguments.push_back(child);
    }
    return result;
}

run_parts builder::run_parts_of(form_id run) const
{
    auto result = run_parts();
    auto index = 0;
    for (auto child = at(run).first_child; is_valid(child); child = at(child).next_sibling, ++index)
    {
        if (index % 2 == 1 && at(child).kind == form_kind::op)
            result.operators.push_back(child);
        else
            result.operands.push_back(child);
    }
    return result;
}

operator_level builder::level_of(form_id op) const
{
    auto const text = token_text_of(op);
    switch (file.at(at(op).token).kind)
    {
    case token_kind::colon:
        return operator_level::ascription;
    case token_kind::arrow:
        return operator_level::arrow;
    case token_kind::double_arrow:
        return operator_level::computes_as;
    case token_kind::symbol:
        return text == "and" || text == "or" ? operator_level::connective : operator_level::ascription;
    default:
        break;
    }

    // The same reading of a spelling the form parser used to place the operator.
    if (is_comparison(text))
        return operator_level::comparison;
    if (text.starts_with(".."))
        return operator_level::range;
    if (text.ends_with('='))
        return operator_level::assignment;
    return operator_level::arithmetic;
}

bool builder::is_keyword_led(form_id id) const
{
    return is_kind(id, form_kind::keyword_form) && is_kind(at(id).first_child, form_kind::keyword);
}

bool builder::is_keyword_led(form_id id, cc::string_view keyword) const
{
    return is_keyword_led(id) && token_text_of(at(id).first_child) == keyword;
}

bool builder::is_binary_run(form_id id, cc::string_view spelling) const
{
    if (!is_kind(id, form_kind::operator_run))
        return false;
    auto const parts = run_parts_of(id);
    return parts.operands.size() == 2 && parts.operators.size() == 1 && token_text_of(parts.operators[0]) == spelling;
}

bool builder::is_arm_run(run_parts const& parts) const
{
    return parts.operands.size() == 2 && parts.operators.size() == 1
        && level_of(parts.operators[0]) == operator_level::computes_as && !is_keyword_led(parts.operands[0]);
}

bool builder::is_assignment_run(run_parts const& parts) const
{
    return parts.operands.size() == 2 && parts.operators.size() == 1
        && level_of(parts.operators[0]) == operator_level::assignment;
}

statement_head builder::head_of(form_id statement) const
{
    auto head = statement_head{.whole = statement};
    if (is_keyword_led(statement))
    {
        head.keyword_form = statement;
        return head;
    }
    if (!is_kind(statement, form_kind::operator_run))
        return head;

    auto const outer = run_parts_of(statement);
    if (outer.operands.size() != 2 || outer.operators.size() != 1)
        return head;
    auto const outer_level = level_of(outer.operators[0]);
    if (outer_level != operator_level::assignment && outer_level != operator_level::computes_as)
        return head;

    if (is_keyword_led(outer.operands[0]))
    {
        head.keyword_form = outer.operands[0];
        if (outer_level == operator_level::computes_as)
        {
            head.arrow_operator = outer.operators[0];
            head.arrow = outer.operands[1];
        }
        else
        {
            head.assign_operator = outer.operators[0];
            head.assign_value = outer.operands[1];
        }
        return head;
    }

    if (outer_level != operator_level::assignment || !is_kind(outer.operands[0], form_kind::operator_run))
        return head;
    auto const inner = run_parts_of(outer.operands[0]);
    auto const is_arrow = inner.operands.size() == 2 && inner.operators.size() == 1
                       && level_of(inner.operators[0]) == operator_level::computes_as;
    if (!is_arrow || !is_keyword_led(inner.operands[0]))
        return head;

    head.keyword_form = inner.operands[0];
    head.arrow_operator = inner.operators[0];
    head.arrow = inner.operands[1];
    head.assign_operator = outer.operators[0];
    head.assign_value = outer.operands[1];
    return head;
}

cc::vector<form_id> builder::lines_of(form_id block)
{
    auto result = cc::vector<form_id>();
    for (auto line = at(block).first_child; is_valid(line); line = at(line).next_sibling)
    {
        if (at(line).kind != form_kind::sequence)
        {
            result.push_back(line);
            continue;
        }
        reject_attributes(line);
        for (auto part = at(line).first_child; is_valid(part); part = at(part).next_sibling)
            result.push_back(part);
    }
    return result;
}

void builder::report(diagnostic_kind kind, source_span where)
{
    ast.diagnostics.push_back({.kind = kind, .level = default_severity_of(kind), .where = where});
}

void builder::report(diagnostic_kind kind, form_id where)
{
    auto const keyword_form = head_of(where).keyword_form;
    if (is_valid(keyword_form))
        report(kind, at(at(keyword_form).first_child).where);
    else if (at(where).kind == form_kind::keyword_form && is_valid(at(where).first_child))
        report(kind, at(at(where).first_child).where);
    else
        report(kind, at(where).where);
}

// ---- parts ----------------------------------------------------------------------------------------------------

range_of<attribute> builder::attributes_of(form_id id)
{
    auto const& f = at(id);
    auto collected = cc::vector<attribute>();
    for (auto i = f.first_attribute; i < f.first_attribute + f.attribute_count; ++i)
    {
        auto const group = file.form_attributes[i];
        auto const where = file.at(file.at(group).token).where;
        // The token is `@name`.
        auto const name = where.length > 0 ? source_span{.offset = where.offset + 1, .length = where.length - 1} : where;
        auto const list = file.form_attribute_arguments[i];
        auto const arguments = is_valid(list) ? list_elements(list) : range_of<argument>();
        collected.push_back({.group = group, .name = name, .list = list, .arguments = arguments});
    }
    return append(ast.attributes, cc::span<attribute const>(collected));
}

void builder::reject_attributes(form_id id)
{
    if (at(id).attribute_count > 0)
        report(diagnostic_kind::misplaced_attribute_on_expression, id);
}

argument builder::list_element(form_id element, bool is_object, bool allows_attributes)
{
    auto result = argument{.form = element, .attributes = attributes_of(element)};
    if (!allows_attributes)
        reject_attributes(element);

    if (is_kind(element, form_kind::prefix_operator) && token_text_of(element) == "..")
    {
        result.is_splat = true;
        result.value = expression(at(element).first_child);
    }
    else if (is_binary_run(element, "="))
    {
        auto const parts = run_parts_of(element);
        if (is_kind(parts.operands[0], form_kind::identifier))
        {
            result.name = at(parts.operands[0]).where;
            result.value = expression(parts.operands[1]);
        }
        else if (is_kind(parts.operands[0], form_kind::leading_dot))
        {
            result.name = file.at(at(parts.operands[0]).token).where;
            result.is_dotted_name = true;
            result.value = expression(parts.operands[1]);
        }
        else
            result.value = invalid_expression(element, diagnostic_kind::expected_name);
    }
    else if (is_object && is_kind(element, form_kind::identifier))
    {
        result.name = at(element).where;
        result.is_shorthand = true;
    }
    else
        result.value = expression(element, attribute_mode::taken);
    return result;
}

range_of<argument> builder::list_elements(form_id list, bool is_object, bool allows_attributes)
{
    auto collected = cc::vector<argument>();
    for (auto element = at(list).first_child; is_valid(element); element = at(element).next_sibling)
        collected.push_back(list_element(element, is_object, allows_attributes));
    return append(ast.arguments, cc::span<argument const>(collected));
}

bool builder::is_field_like(form_id element, bool needs_type) const
{
    auto target = element;
    if (is_binary_run(target, "="))
        target = at(target).first_child;

    auto has_type = false;
    if (is_kind(target, form_kind::operator_run))
    {
        auto const parts = run_parts_of(target);
        auto const is_typed = parts.operators.size() + 1 == parts.operands.size() && !parts.operators.empty()
                           && token_text_of(parts.operators[0]) == ":";
        if (!is_typed)
            return false;
        has_type = true;
        target = parts.operands[0];
    }
    if (needs_type && !has_type)
        return false;

    if (is_kind(target, form_kind::identifier) || is_kind(target, form_kind::wildcard)
        || is_kind(target, form_kind::leading_dot))
        return true;
    if (!is_keyword_led(target, "mut"))
        return false;
    auto const parts = keyword_parts_of(target);
    return parts.keywords.size() == 1 && parts.arguments.size() == 1 && !is_valid(parts.block)
        && is_kind(parts.arguments[0], form_kind::identifier);
}

field builder::make_field(form_id element, diagnostic_kind on_failure)
{
    auto result = field{.form = element, .attributes = attributes_of(element)};
    if (!is_field_like(element, false))
    {
        result.type = invalid_expression(element, on_failure);
        return result;
    }

    auto target = element;
    if (is_binary_run(target, "="))
    {
        auto const parts = run_parts_of(target);
        result.default_value = expression(parts.operands[1]);
        target = parts.operands[0];
    }
    if (is_kind(target, form_kind::operator_run))
    {
        auto const parts = run_parts_of(target);
        result.type = type_after_first_operator(target, parts);
        target = parts.operands[0];
    }
    if (is_keyword_led(target))
    {
        result.is_mut = true;
        target = keyword_parts_of(target).arguments[0];
    }
    if (is_kind(target, form_kind::leading_dot))
    {
        result.is_named_only = true;
        result.name = file.at(at(target).token).where;
        return result;
    }
    result.name = at(target).where;
    return result;
}

range_of<field> builder::fields_of(form_id list, diagnostic_kind on_failure)
{
    auto collected = cc::vector<field>();
    for (auto element = at(list).first_child; is_valid(element); element = at(element).next_sibling)
        collected.push_back(make_field(element, on_failure));
    return append(ast.fields, cc::span<field const>(collected));
}

// ---- entry ----------------------------------------------------------------------------------------------------

void builder::reject_reserved_names()
{
    // AST-141: every name a declaration takes, once the whole file is read
    auto const is_reserved = [&](source_span name, bool allows_self)
    {
        auto const text = file.text_of(name);
        return text == "void" || (!allows_self && text == "self");
    };
    for (auto const& d : ast.decls)
        d.node.visit(
            [&](auto const& n)
            {
                if constexpr (requires { n.name.offset; })
                {
                    if (is_reserved(n.name, false))
                        report(diagnostic_kind::reserved_name, n.name);
                }
            });
    // a parameter may be `self`, the receiver of a method, and is otherwise held like a field
    for (auto const& f : ast.fields)
        if (is_reserved(f.name, true))
            report(diagnostic_kind::reserved_name, f.name);
}

file_ast sgl::ast::build(parsed_file const& file)
{
    auto b = builder{.file = file, .ast = {}};
    if (is_valid(file.root_form))
        b.ast.declarations = b.declarations(file.root_form, scope_kind::file);
    b.reject_reserved_names();
    return cc::move(b.ast);
}
