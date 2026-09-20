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
        if (s.intrinsic != builtin::none)
            out += " builtin";
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

    /// `is_broken` gives every argument of a construction a line of its own, which is how a returned struct reads.
    void dump_expr(flat_entry_point const& e, flat_expr_id id, bool is_broken = false)
    {
        if (!is_valid(id))
        {
            out += "<none>";
            return;
        }
        auto const& x = e.at(id);
        auto const arguments = [&](ast::range_of<flat_expr_id> range)
        {
            for (auto const a : e.at(range))
            {
                out += is_broken ? "\n    " : " ";
                dump_expr(e, a);
            }
        };

        x.node.visit([&](flat_invalid const&) { out += "(invalid"; },
                     [&](flat_literal const& l)
                     {
                         // `1.0` and not `1`: the dump says float where the type is not in view
                         auto const text = cc::format("{}", l.value);
                         auto const is_whole = !text.contains('.') && !text.contains('e') && !text.contains('n');
                         out.appendf("(lit {}{}", text, is_whole ? ".0" : "");
                     },
                     [&](flat_local_ref const& l) { out.appendf("(local {}", e.at(l.local).name); },
                     [&](flat_binding_member const& b)
                     {
                         auto const& binding = m.bindings[m.at(b.binding).info];
                         out.appendf("(binding {} {}", m.at(b.binding).name, m.at(binding.members)[b.member].name);
                     },
                     [&](flat_member const& member)
                     {
                         out += "(member ";
                         dump_expr(e, member.object);
                         auto const& object_type = m.at(e.at(member.object).type);
                         out.appendf(" {}", m.at(object_type.members)[member.member].name);
                     },
                     [&](flat_construct const& c)
                     {
                         out += "(construct";
                         arguments(c.arguments);
                     },
                     [&](flat_call const& c)
                     {
                         out.appendf("(call {}", m.at(c.callee).name);
                         arguments(c.arguments);
                     });
        out.appendf(" : {})", m.name_of(x.type));
    }

    void dump_entry_point(flat_entry_point const& e)
    {
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

        for (auto const id : e.at(e.body))
        {
            out += "\n  ";
            auto const& s = e.at(id);
            s.node.visit(
                [&](flat_let const& let)
                {
                    auto const& local = e.at(let.local);
                    out.appendf("(let{} {} : {} = ", local.kind == local_kind::temporary ? ":temporary" : "",
                                local.name, m.name_of(local.type));
                    dump_expr(e, let.value);
                    out += ")";
                },
                [&](flat_return const& r)
                {
                    out += "(return ";
                    dump_expr(e, r.value, true);
                    out += ")";
                });
        }
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
