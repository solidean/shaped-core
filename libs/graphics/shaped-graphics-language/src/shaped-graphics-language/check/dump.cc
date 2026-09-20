#include "dump.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;

cc::string_view stage_name(stage s)
{
    switch (s)
    {
    case stage::none:
        return "";
    case stage::vertex:
        return "vertex";
    case stage::pixel:
        return "pixel";
    }
    return "";
}

struct dumper
{
    checked_module const& m;
    cc::string out;

    void members(ast::range_of<member_info> range)
    {
        for (auto const& member : m.at(range))
            out.appendf(" ({}{} : {})", member.name, member.is_position ? "{@position}" : "", m.name_of(member.type));
    }

    void signature(function_info const& f)
    {
        for (auto const& p : m.at(f.parameters))
            out.appendf(" ({} : {})", p.name, m.name_of(p.type));
        if (!f.bindings.empty())
        {
            out += " (uses";
            for (auto const b : m.at(f.bindings))
                out.appendf(" {}", m.at(b).name);
            out += ")";
        }
        out.appendf(" -> {}", m.name_of(f.result));
    }

    void dump_symbol(symbol const& s)
    {
        switch (s.kind)
        {
        case symbol_kind::structure:
            out += "(struct ";
            break;
        case symbol_kind::enumeration:
            out += "(enum ";
            break;
        case symbol_kind::function:
            out += "(fun ";
            break;
        case symbol_kind::binding:
            out += "(binding ";
            break;
        case symbol_kind::unsupported:
            out += "(unsupported ";
            break;
        }
        out += s.name;

        if (s.state != symbol_state::checked)
        {
            out += s.state == symbol_state::failed ? " failed)\n" : " unfinished)\n";
            return;
        }
        if (is_valid(s.intrinsic) || is_valid(s.intrinsic_type))
            out += " builtin";
        if (s.kind == symbol_kind::function && m.functions[s.info].is_pure)
            out += " pure";
        if (!s.operator_spelling.empty())
            out.appendf(" operator:{}", s.operator_spelling);

        if (s.kind == symbol_kind::structure)
        {
            auto const& type = m.at(s.type);
            if (type.edge != stage::none)
                out.appendf(" {}", stage_name(type.edge));
            if (type.is_opaque)
                out += " opaque";
            members(type.members);
        }
        else if (s.kind == symbol_kind::enumeration)
        {
            for (auto const& c : m.at(m.at(s.type).cases))
                out.appendf(" ({} = {})", c.name, c.value);
        }
        else if (s.kind == symbol_kind::binding)
        {
            auto const& b = m.bindings[s.info];
            if (b.is_inline)
                out += " inline";
            members(b.members);
        }
        else if (s.kind == symbol_kind::function)
        {
            auto const& f = m.functions[s.info];
            if (f.entry_stage != stage::none)
                out.appendf(" {}", stage_name(f.entry_stage));
            signature(f);
        }
        out += ")\n";
    }

    void new_line(int indent)
    {
        out += "\n";
        for (auto i = 0; i < indent; ++i)
            out += " ";
    }

    static cc::string_view label_name(flat_entry_point const& e, label_id id)
    {
        return is_valid(id) && index_of(id) < e.labels.size() ? cc::string_view(e.at(id).name) : cc::string_view("?");
    }

    static cc::string_view local_name(flat_entry_point const& e, local_id id)
    {
        return is_valid(id) && index_of(id) < e.locals.size() ? cc::string_view(e.at(id).name) : cc::string_view("?");
    }

    /// `indent` is where the statement that holds the expression starts, which is what a block expression indents from.
    /// `is_broken` gives every argument of a construction a line of its own, which is how a returned struct reads.
    void dump_expr(flat_entry_point const& e, flat_expr_id id, int indent, bool is_broken = false)
    {
        if (!is_valid(id) || index_of(id) >= e.exprs.size())
        {
            out += "<none>";
            return;
        }
        auto const& x = e.at(id);
        auto const arguments = [&](ast::range_of<flat_expr_id> range, bool is_split)
        {
            for (auto const a : e.at(range))
            {
                if (is_split)
                    new_line(indent + 2);
                else
                    out += " ";
                dump_expr(e, a, indent);
            }
        };
        auto const operands = [&](cc::string_view name, flat_expr_id lhs, flat_expr_id rhs)
        {
            out.appendf("({} ", name);
            dump_expr(e, lhs, indent);
            out += " ";
            dump_expr(e, rhs, indent);
        };

        x.node.visit([&](flat_invalid const&) { out += "(invalid"; },
                     [&](flat_literal const& l)
                     {
                         // `1.0` and not `1`: the dump says float where the type is not in view
                         auto const text = cc::format("{}", l.value);
                         auto const is_whole = !text.contains('.') && !text.contains('e') && !text.contains('n');
                         out.appendf("(lit {}{}", text, is_whole ? ".0" : "");
                     },
                     [&](flat_int_literal const& l) { out.appendf("(lit {}", l.value); },
                     [&](flat_bool_literal const& l) { out.appendf("(lit {}", l.value ? "true" : "false"); },
                     [&](flat_enum_value const& v)
                     {
                         auto const cases = m.at(m.at(x.type).cases);
                         auto const name = v.case_index >= 0 && v.case_index < cases.size()
                                             ? cc::string_view(cases[v.case_index].name)
                                             : cc::string_view("?");
                         out.appendf("(case .{}", name);
                     },
                     [&](flat_local_ref const& l) { out.appendf("(local {}", local_name(e, l.local)); },
                     [&](flat_binding_member const& b)
                     {
                         auto const& binding = m.bindings[m.at(b.binding).info];
                         out.appendf("(binding {} {}", m.at(b.binding).name, m.at(binding.members)[b.member].name);
                     },
                     [&](flat_member const& member)
                     {
                         out += "(member ";
                         dump_expr(e, member.object, indent);
                         auto name = cc::string_view("?");
                         if (is_valid(member.object) && index_of(member.object) < e.exprs.size())
                         {
                             auto const fields = m.at(m.at(e.at(member.object).type).members);
                             if (member.member >= 0 && member.member < fields.size())
                                 name = fields[member.member].name;
                         }
                         out.appendf(" {}", name);
                     },
                     [&](flat_construct const& c)
                     {
                         out += "(construct";
                         arguments(c.arguments, is_broken);
                     },
                     [&](flat_call const& c)
                     {
                         out.appendf("(call{} {}", c.is_pure ? "" : ":effect", m.at(c.callee).name);
                         arguments(c.arguments, false);
                     },
                     [&](flat_not const& n)
                     {
                         out += "(not ";
                         dump_expr(e, n.operand, indent);
                     },
                     [&](flat_and const& a) { operands("and", a.lhs, a.rhs); },
                     [&](flat_or const& o) { operands("or", o.lhs, o.rhs); },
                     [&](flat_block const& b)
                     {
                         out.appendf("(block ${}", label_name(e, b.label));
                         dump_body(e, b.body, indent + 4);
                     });
        out.appendf(" : {})", m.name_of(x.type));
    }

    void dump_body(flat_entry_point const& e, ast::range_of<flat_stmt_id> body, int indent)
    {
        for (auto const id : e.at(body))
        {
            new_line(indent);
            dump_stmt(e, id, indent);
        }
    }

    void dump_declaration(flat_entry_point const& e, cc::string_view keyword, local_id id, flat_expr_id value, int indent)
    {
        auto const is_known = is_valid(id) && index_of(id) < e.locals.size();
        auto const is_temporary = is_known && e.at(id).kind == local_kind::temporary;
        auto const type = is_known ? e.at(id).type : checked_module::error_type;
        out.appendf("({}{} {} : {}", keyword, is_temporary ? ":temporary" : "", local_name(e, id), m.name_of(type));
        if (is_valid(value))
        {
            out += " = ";
            dump_expr(e, value, indent);
        }
        out += ")";
    }

    void dump_arms(flat_entry_point const& e,
                   cc::string_view head,
                   flat_expr_id scrutinee,
                   ast::range_of<flat_arm> arms,
                   ast::range_of<flat_stmt_id> default_body,
                   int indent)
    {
        out.appendf("({} ", head);
        dump_expr(e, scrutinee, indent, true);
        for (auto const& arm : e.at(arms))
        {
            new_line(indent + 2);
            out += "(arm";
            for (auto const p : e.at(arm.patterns))
            {
                out += " ";
                dump_expr(e, p, indent + 2, true);
            }
            dump_body(e, arm.body, indent + 4);
            out += ")";
        }
        new_line(indent + 2);
        out += "(default";
        dump_body(e, default_body, indent + 4);
        out += "))";
    }

    void dump_stmt(flat_entry_point const& e, flat_stmt_id id, int indent)
    {
        auto const& s = e.at(id);
        s.node.visit([&](flat_let const& let) { dump_declaration(e, "let", let.local, let.value, indent); },
                     [&](flat_var const& var) { dump_declaration(e, "var", var.local, var.value, indent); },
                     [&](flat_assign const& a)
                     {
                         out += "(assign ";
                         dump_expr(e, a.place, indent);
                         out += " = ";
                         dump_expr(e, a.value, indent);
                         out += ")";
                     },
                     [&](flat_print const& p)
                     {
                         out += "(print ";
                         dump_expr(e, p.value, indent);
                         out += ")";
                     },
                     [&](flat_eval const& v)
                     {
                         out += "(eval ";
                         dump_expr(e, v.value, indent);
                         out += ")";
                     },
                     [&](flat_if const& i)
                     {
                         out += "(if ";
                         dump_expr(e, i.condition, indent);
                         new_line(indent + 2);
                         out += "(then";
                         dump_body(e, i.then_body, indent + 4);
                         out += ")";
                         if (!i.else_body.empty())
                         {
                             new_line(indent + 2);
                             out += "(else";
                             dump_body(e, i.else_body, indent + 4);
                             out += ")";
                         }
                         out += ")";
                     },
                     [&](flat_block const& b)
                     {
                         out.appendf("(block ${}", label_name(e, b.label));
                         dump_body(e, b.body, indent + 2);
                         out += ")";
                     },
                     [&](flat_leave const& l)
                     {
                         out.appendf("(leave ${}", label_name(e, l.target));
                         if (is_valid(l.value))
                         {
                             out += " ";
                             dump_expr(e, l.value, indent, true);
                         }
                         out += ")";
                     },
                     [&](flat_loop const& l)
                     {
                         out.appendf("(loop ${}", label_name(e, l.label));
                         dump_body(e, l.body, indent + 2);
                         out += ")";
                     },
                     [&](flat_while const& w)
                     {
                         out.appendf("(while ${} ", label_name(e, w.label));
                         dump_expr(e, w.condition, indent);
                         dump_body(e, w.body, indent + 2);
                         out += ")";
                     },
                     [&](flat_for const& f)
                     {
                         out.appendf("(for ${} {} in ", label_name(e, f.label), local_name(e, f.index));
                         dump_expr(e, f.first, indent);
                         out += " ..< ";
                         dump_expr(e, f.end, indent);
                         dump_body(e, f.body, indent + 2);
                         out += ")";
                     },
                     [&](flat_continue const& c) { out.appendf("(continue ${})", label_name(e, c.target)); },
                     [&](flat_once const& o)
                     {
                         out += "(once";
                         dump_body(e, o.body, indent + 2);
                         out += ")";
                     },
                     [&](flat_break const&) { out += "(break)"; },
                     [&](flat_case const& c) { dump_arms(e, "case", c.scrutinee, c.arms, c.default_body, indent); },
                     [&](flat_switch const& c) { dump_arms(e, "switch", c.scrutinee, c.arms, c.default_body, indent); },
                     [&](flat_return const& r)
                     {
                         out += "(return ";
                         dump_expr(e, r.value, indent, true);
                         out += ")";
                     });
    }

    void dump_entry_point(flat_entry_point const& e)
    {
        if (e.locals.empty())
        {
            out.appendf("(entry {} {} <no parameter>)\n", stage_name(e.entry_stage), e.name);
            return;
        }
        auto const& parameter = e.locals.front();
        out.appendf("(entry {} {} ({} : {})", stage_name(e.entry_stage), e.name, parameter.name,
                    m.name_of(parameter.type));
        if (!e.bindings.empty())
        {
            out += " (uses";
            for (auto const b : e.bindings)
                out.appendf(" {}", m.at(b).name);
            out += ")";
        }
        out.appendf(" -> {}", m.name_of(e.result));
        dump_body(e, e.body, 2);
        out += ")\n";
    }
};
} // namespace

cc::string sgl::check::dump(checked_module const& m)
{
    auto d = dumper{.m = m};
    for (auto const& s : m.symbols)
        d.dump_symbol(s);
    for (auto const& e : m.entry_points)
        d.dump_entry_point(e);
    return d.out;
}

cc::string sgl::check::dump_entry_points(checked_module const& m)
{
    auto d = dumper{.m = m};
    for (auto const& e : m.entry_points)
        d.dump_entry_point(e);
    return d.out;
}

cc::string sgl::check::dump_entry_point(checked_module const& m, flat_entry_point const& e)
{
    auto d = dumper{.m = m};
    d.dump_entry_point(e);
    return d.out;
}

cc::string sgl::check::dump_diagnostics(checked_module const& m)
{
    auto out = cc::string();
    for (auto const& d : m.diagnostics)
    {
        out.appendf("{} @{}:{}+{}", to_string(d.what.kind), d.file, d.what.where.offset, d.what.where.length);
        if (!d.detail.empty())
            out.appendf(" {}", d.detail);
        out += "\n";
    }
    return out;
}
