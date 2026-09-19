#include "dump.hh"

#include <clean-core/common/assert.hh>

namespace
{
using namespace sgl;

void indent(cc::string& out, int depth)
{
    for (auto i = 0; i < depth; ++i)
        out += "  ";
}

void dump_group(parsed_file const& file, cc::string& out, i32 index, int depth);

void dump_attribute(parsed_file const& file, cc::string& out, i32 attribute)
{
    out += "{";
    out += file.text_of(file.tokens[file.groups[attribute].token].where);
    if (file.groups[attribute].first_child >= 0)
        dump_group(file, out, file.groups[attribute].first_child, 0);
    out += "}";
}

void dump_group_run(parsed_file const& file, cc::string& out, i32 first, int depth)
{
    for (auto index = first; index >= 0; index = file.groups[index].next_sibling)
    {
        auto const& g = file.groups[index];
        if (g.starts_line)
            out += index == first ? "| " : " | ";
        else if (index != first && g.kind != group_kind::block)
            out += g.is_fused_left ? "~" : " ";
        dump_group(file, out, index, depth);
    }
}

void dump_group(parsed_file const& file, cc::string& out, i32 index, int depth)
{
    auto const& g = file.groups[index];
    switch (g.kind)
    {
    case group_kind::token:
        out += file.text_of(file.tokens[g.token].where);
        break;

    case group_kind::round:
    case group_kind::square:
    case group_kind::curly:
        out += file.text_of(file.tokens[g.token].where);
        dump_group_run(file, out, g.first_child, depth);
        out += g.close_token >= 0 ? file.text_of(file.tokens[g.close_token].where) : cc::string_view("<unclosed>");
        break;

    case group_kind::quoted:
        out += "\"";
        for (auto child = g.first_child; child >= 0; child = file.groups[child].next_sibling)
        {
            if (child != g.first_child && file.groups[child].starts_line)
                out += "\\n";
            dump_group(file, out, child, depth);
        }
        out += "\"";
        break;

    case group_kind::block:
        out += ":";
        for (auto statement = g.first_child; statement >= 0; statement = file.groups[statement].next_sibling)
        {
            if (file.groups[statement].first_child < 0)
                continue; // a line of attributes only
            out += "\n";
            indent(out, depth + 1);
            dump_group_run(file, out, file.groups[statement].first_child, depth + 1);
        }
        break;

    case group_kind::statement:
    case group_kind::attribute:
        break;
    }
    for (auto a = g.first_attribute; a >= 0; a = file.groups[a].next_sibling)
        dump_attribute(file, out, a);
}

cc::string_view tag_of(form_kind kind)
{
    switch (kind)
    {
    case form_kind::missing:
        return "missing";
    case form_kind::error:
        return "error";
    case form_kind::number:
        return "num";
    case form_kind::quoted:
        return "str";
    case form_kind::hash_literal:
        return "hash";
    case form_kind::identifier:
        return "id";
    case form_kind::wildcard:
        return "wildcard";
    case form_kind::keyword:
        return "kw";
    case form_kind::op:
        return "op";
    case form_kind::round_list:
        return "round";
    case form_kind::square_list:
        return "square";
    case form_kind::curly_list:
        return "curly";
    case form_kind::leading_dot:
        return "dot";
    case form_kind::member:
        return "member";
    case form_kind::call:
        return "call";
    case form_kind::application:
        return "apply";
    case form_kind::prefix_operator:
        return "prefix";
    case form_kind::postfix_operator:
        return "postfix";
    case form_kind::operator_run:
        return "run";
    case form_kind::keyword_form:
        return "kw";
    case form_kind::block:
        return "block";
    case form_kind::sequence:
        return "seq";
    }
    CC_UNREACHABLE("unknown form_kind");
}

bool is_leaf_kind(form_kind kind)
{
    switch (kind)
    {
    case form_kind::missing:
    case form_kind::error:
    case form_kind::number:
    case form_kind::quoted:
    case form_kind::hash_literal:
    case form_kind::identifier:
    case form_kind::wildcard:
    case form_kind::keyword:
    case form_kind::op:
    case form_kind::leading_dot:
        return true;
    default:
        return false;
    }
}

void dump_form(parsed_file const& file, cc::string& out, i32 index, int depth)
{
    auto const& f = file.forms[index];
    if (f.kind == form_kind::block)
    {
        for (auto child = f.first_child; child >= 0; child = file.forms[child].next_sibling)
        {
            out += "\n";
            indent(out, depth + 1);
            dump_form(file, out, child, depth + 1);
        }
        return;
    }

    if (is_leaf_kind(f.kind) && f.first_child < 0)
    {
        out += tag_of(f.kind);
        if (f.kind != form_kind::missing)
        {
            out += ":";
            out += file.text_of(f.kind == form_kind::leading_dot ? file.tokens[f.token].where : f.where);
        }
    }
    else
    {
        out += "(";
        out += tag_of(f.kind);
        auto const names_its_token = f.kind == form_kind::member || f.kind == form_kind::prefix_operator
                                  || f.kind == form_kind::postfix_operator;
        if (names_its_token)
        {
            out += " ";
            out += file.text_of(file.tokens[f.token].where);
        }
        for (auto child = f.first_child; child >= 0; child = file.forms[child].next_sibling)
        {
            if (file.forms[child].kind != form_kind::block)
                out += " ";
            dump_form(file, out, child, depth);
        }
        out += ")";
    }

    for (auto i = f.first_attribute; i < f.first_attribute + f.attribute_count; ++i)
        dump_attribute(file, out, file.form_attributes[i]);
}
} // namespace

cc::string sgl::dump_groups(parsed_file const& file)
{
    auto out = cc::string();
    if (file.root_block < 0)
        return out;

    auto const& root = file.groups[file.root_block];
    for (auto statement = root.first_child; statement >= 0; statement = file.groups[statement].next_sibling)
    {
        if (file.groups[statement].first_child < 0)
            continue; // a line of attributes only
        dump_group_run(file, out, file.groups[statement].first_child, 0);
        out += "\n";
    }
    return out;
}

cc::string sgl::dump_forms(parsed_file const& file)
{
    auto out = cc::string();
    if (file.root_form < 0)
        return out;

    for (auto child = file.forms[file.root_form].first_child; child >= 0; child = file.forms[child].next_sibling)
    {
        dump_form(file, out, child, 0);
        out += "\n";
    }
    return out;
}
