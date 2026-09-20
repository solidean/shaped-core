#include "dialect.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// How tightly an expression holds together, loosest first; the same ladder in every target so far.
enum class level : u8
{
    additive,
    multiplicative,
    /// A negative literal.
    unary,
    /// A name, a call, a construction, a member access: whatever needs no parentheses anywhere.
    primary,
};

struct rendered
{
    cc::string text;
    level binds = level::primary;
};

/// Shortest text that reads back as `value`, always with a decimal point: `1.0`, `0.45`, `1.0e+20`.
/// `value` must be finite.
cc::string literal_text(f64 value)
{
    auto text = cc::to_string(value);
    if (text.contains('.'))
        return text;
    auto const exponent = text.find('e');
    if (exponent < 0)
        text += ".0";
    else
        text.insert_range_at(exponent, ".0");
    return text;
}

struct writer
{
    plan& p;
    dialect const& d;
    cc::string out;

    void line(cc::string_view text)
    {
        out += k_indent;
        out += text;
        out += "\n";
    }

    static cc::string wrapped(rendered r, level needed)
    {
        return r.binds < needed ? cc::format("({})", r.text) : cc::move(r.text);
    }

    /// Left-associative, so an equal level on the right keeps its parentheses: `a + (b + c)` is not `a + b + c` in floats.
    static rendered binary(cc::string_view op, level own, rendered lhs, rendered rhs)
    {
        auto const tighter = level(u8(own) + 1);
        return {.text = cc::format("{} {} {}", wrapped(cc::move(lhs), own), op, wrapped(cc::move(rhs), tighter)),
                .binds = own};
    }

    bool needs_member_assignment(flat_expr const& x) const
    {
        return x.node.is<flat_construct>() && !is_builtin_type(p.m, x.type) && !d.has_struct_constructor();
    }

    /// Declares `name` and assigns its members one by one, for a target without a struct constructor.
    void build_struct(flat_expr const& x, cc::string_view name)
    {
        auto declaration = cc::string();
        d.write_local(declaration, {.name = name, .type = type_text(p, d, x.type), .is_mut = true});
        line(declaration);

        auto const& members = p.structs[p.struct_of_type[index_of(x.type)]].members;
        auto const arguments = p.e.at(x.node.as<flat_construct>().arguments);
        for (auto i = isize(0); i < arguments.size() && i < members.size(); ++i)
            line(cc::format("{}.{} = {};", name, members[i].name, expr(arguments[i]).text));
    }

    /// `is_broken` gives every argument a line of its own, which is how a struct built as a statement's value reads.
    rendered construct(flat_expr const& x, bool is_broken)
    {
        if (needs_member_assignment(x))
        {
            // A construction in the middle of an expression: every expression is pure, so building it first changes nothing.
            auto name = p.names.mint(cc::format("{}_value", type_text(p, d, x.type)));
            build_struct(x, name);
            return {.text = cc::move(name)};
        }

        auto const arguments = p.e.at(x.node.as<flat_construct>().arguments);
        auto const is_split = is_broken && arguments.size() > 1;
        auto text = cc::string(type_text(p, d, x.type));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (is_split)
                text.appendf("{}\n{}{}", i == 0 ? "" : ",", k_indent, k_indent);
            else if (i != 0)
                text += ", ";
            text += expr(arguments[i]).text;
        }
        if (is_split)
            text.appendf("\n{}", k_indent);
        text += ")";
        return {.text = cc::move(text)};
    }

    /// `m * (v, w)`, where a target has either an operator or a function for it.
    rendered transformed(flat_call const& c, f64 w)
    {
        auto const arguments = p.e.at(c.arguments);
        auto vector = rendered{
            .text = cc::format("{}({}, {})", d.type_name(builtin::float4), expr(arguments[1]).text, literal_text(w))};
        auto matrix = expr(arguments[0]);
        if (d.has_mul_function())
            return {.text = cc::format("mul({}, {})", matrix.text, vector.text)};
        return binary("*", level::multiplicative, cc::move(matrix), cc::move(vector));
    }

    rendered call(flat_call const& c)
    {
        auto const arguments = p.e.at(c.arguments);
        switch (c.intrinsic)
        {
        case builtin::transform_position:
            return transformed(c, 1.0);
        case builtin::transform_direction:
            return {.text = cc::format("{}.xyz", wrapped(transformed(c, 0.0), level::primary))};
        case builtin::scale_color:
        case builtin::multiply:
            return binary("*", level::multiplicative, expr(arguments[0]), expr(arguments[1]));
        case builtin::add:
            return binary("+", level::additive, expr(arguments[0]), expr(arguments[1]));
        default:
            break;
        }

        auto text = cc::string(d.function_name(c.intrinsic));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (i != 0)
                text += ", ";
            text += expr(arguments[i]).text;
        }
        text += ")";
        return {.text = cc::move(text)};
    }

    rendered expr(flat_expr_id id, bool is_broken = false)
    {
        auto const& x = p.e.at(id);
        auto result = rendered();
        x.node.visit([&](flat_invalid const&) {}, [&](flat_literal const& l)
                     { result = {.text = literal_text(l.value), .binds = l.value < 0 ? level::unary : level::primary}; },
                     [&](flat_local_ref const& l) { result = {.text = p.locals[index_of(l.local)]}; },
                     [&](flat_binding_member const& b)
                     {
                         auto const& constants = p.constants.value();
                         result = {.text = cc::format("{}.{}", constants.name, constants.members[b.member].name)};
                     },
                     [&](flat_member const& member)
                     {
                         auto const object_type = p.e.at(member.object).type;
                         auto object = wrapped(expr(member.object), level::primary);
                         // A builtin's fields are spelled alike everywhere: x, y, z, w.
                         auto const name
                             = is_builtin_type(p.m, object_type)
                                 ? cc::string_view(p.m.at(p.m.at(object_type).members)[member.member].name)
                                 : cc::string_view(
                                       p.structs[p.struct_of_type[index_of(object_type)]].members[member.member].name);
                         result = {.text = cc::format("{}.{}", object, name)};
                     },
                     [&](flat_construct const&) { result = construct(x, is_broken); },
                     [&](flat_call const& c) { result = call(c); });
        return result;
    }

    void statement(flat_stmt const& s)
    {
        s.node.visit(
            [&](flat_let const& let)
            {
                auto const& local = p.e.at(let.local);
                auto const name = cc::string_view(p.locals[index_of(let.local)]);
                auto const& value = p.e.at(let.value);
                if (needs_member_assignment(value))
                {
                    build_struct(value, name);
                    return;
                }
                auto const text = expr(let.value, true).text;
                auto declaration = cc::string();
                d.write_local(declaration,
                              {.name = name, .type = type_text(p, d, local.type), .value = text, .is_mut = local.is_mut});
                line(declaration);
            },
            [&](flat_return const& r)
            {
                auto const& value = p.e.at(r.value);
                if (needs_member_assignment(value))
                {
                    auto const name = p.names.mint("result");
                    build_struct(value, name);
                    line(cc::format("return {};", name));
                    return;
                }
                line(cc::format("return {};", expr(r.value, true).text));
            });
    }
};
} // namespace

cc::string_view sgl::emit::impl::type_text(plan const& p, dialect const& d, check::type_id type)
{
    if (is_builtin_type(p.m, type))
        return d.type_name(builtin_of_type(p.m, type));
    return p.structs[p.struct_of_type[index_of(type)]].name;
}

cc::string_view sgl::emit::impl::stage_name(check::stage s)
{
    switch (s)
    {
    case check::stage::none:
        return "";
    case check::stage::vertex:
        return "vertex";
    case check::stage::pixel:
        return "pixel";
    }
    return "";
}

cc::string sgl::emit::impl::write_text(plan& p, dialect const& d)
{
    auto w = writer{.p = p, .d = d};
    w.out.appendf("// SGL {} entry point '{}', written as {}.\n", stage_name(p.e.entry_stage), p.e.name, d.description());
    w.out += "// Generated: the SGL source is what to edit.\n\n";
    d.write_declarations(w.out, p);
    d.write_function_head(w.out, p);
    for (auto const id : p.e.at(p.e.body))
        w.statement(p.e.at(id));
    w.out += "}\n";
    return cc::move(w.out);
}
