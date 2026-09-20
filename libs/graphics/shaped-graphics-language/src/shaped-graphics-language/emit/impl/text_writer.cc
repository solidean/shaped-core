#include "dialect.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

using level = builtins::precedence;
using rendered = builtins::written;

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

/// `-2147483648` is no literal anywhere: it is a minus in front of a number that does not fit.
cc::string int_literal_text(i32 value)
{
    if (value == -2147483647 - 1)
        return "(-2147483647 - 1)";
    return cc::to_string(value);
}

struct writer
{
    plan& p;
    dialect const& d;
    cc::string out;
    /// How many levels the next line is indented by; the body of the function is level 1.
    int depth = 1;

    static void indent(cc::string& text, int levels)
    {
        for (auto i = 0; i < levels; ++i)
            text += k_indent;
    }

    void line(cc::string_view text)
    {
        indent(out, depth);
        out += text;
        out += "\n";
    }

    /// `head` and the brace that opens its body, where the target puts it.
    void open(cc::string_view head)
    {
        if (d.is_c_like())
        {
            line(head);
            line("{");
        }
        else
            line(cc::format("{} {{", head));
        ++depth;
    }

    /// Closes one body and opens the next under `head`: an `else`.
    void reopen(cc::string_view head)
    {
        --depth;
        if (d.is_c_like())
        {
            line("}");
            line(head);
            line("{");
        }
        else
            line(cc::format("}} {} {{", head));
        ++depth;
    }

    void close(cc::string_view tail = "")
    {
        --depth;
        line(cc::format("}}{}", tail));
    }

    cc::string condition_text(cc::string_view keyword, cc::string_view condition) const
    {
        return d.is_c_like() ? cc::format("{} ({})", keyword, condition) : cc::format("{} {}", keyword, condition);
    }

    static cc::string wrapped(rendered r, level needed) { return builtins::wrapped(cc::move(r), needed); }

    /// `&&` and `||` never stand bare inside each other: WGSL refuses the mix, and no reader should need the ladder.
    static rendered logical(cc::string_view op, level own, rendered lhs, rendered rhs)
    {
        auto const needed_left = lhs.binds == own ? own : level::comparison;
        return {.text = cc::format("{} {} {}", wrapped(cc::move(lhs), needed_left), op,
                                   wrapped(cc::move(rhs), level::comparison)),
                .binds = own};
    }

    /// True when writing `id` puts lines in front of the statement that holds it: a struct built member by member.
    /// Such an expression cannot stand where it is evaluated more than once, or behind an `else`.
    bool writes_lines(flat_expr_id id) const
    {
        auto const& x = p.e.at(id);
        if (needs_member_assignment(x))
            return true;
        auto result = false;
        check::impl::for_each_operand(p.e, x, [&](flat_expr_id operand) { result = result || writes_lines(operand); });
        return result;
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
        // a vector stays on its line: its arguments are short, and `vec3f(0.45, 0.8, -0.4)` is how a shader reads
        auto const is_split = is_broken && arguments.size() > 1 && !is_builtin_type(p.m, x.type);
        auto text = cc::string(type_text(p, d, x.type));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (is_split)
            {
                text += i == 0 ? "\n" : ",\n";
                indent(text, depth + 1);
            }
            else if (i != 0)
                text += ", ";
            text += expr(arguments[i]).text;
        }
        if (is_split)
        {
            text += "\n";
            indent(text, depth);
        }
        text += ")";
        return {.text = cc::move(text)};
    }

    /// Every call is written from its registry record; nothing here knows one builtin from another.
    rendered call(flat_call const& c)
    {
        auto const& record = *p.m.builtin_function(c.intrinsic);
        auto arguments = cc::vector<rendered>();
        for (auto const id : p.e.at(c.arguments))
            arguments.push_back(expr(id));

        auto const& how = record.write;
        switch (how.kind)
        {
        case builtins::spelling_kind::infix:
            return builtins::write_infix(how.text, how.binds, cc::move(arguments[0]), cc::move(arguments[1]));
        case builtins::spelling_kind::prefix:
            // parenthesized whenever it is no name: `--x` is a decrement in every C-like target
            return {.text = cc::format("{}{}", how.text, wrapped(cc::move(arguments[0]), level::primary)),
                    .binds = level::unary};
        case builtins::spelling_kind::custom:
            return how.custom({.target = d.language(), .arguments = arguments, .builtins = *p.m.builtins});
        case builtins::spelling_kind::call:
            break;
        }

        auto text = cc::string(record.called_in(d.language()));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (i != 0)
                text += ", ";
            text += arguments[i].text;
        }
        text += ")";
        return {.text = cc::move(text)};
    }

    rendered expr(flat_expr_id id, bool is_broken = false)
    {
        auto const& x = p.e.at(id);
        auto result = rendered();
        x.node.visit(
            [&](flat_invalid const&) {}, [&](flat_literal const& l)
            { result = {.text = literal_text(l.value), .binds = l.value < 0 ? level::unary : level::primary}; },
            [&](flat_int_literal const& l)
            {
                auto const is_wrapped = l.value == -2147483647 - 1;
                result = {.text = int_literal_text(l.value),
                          .binds = l.value < 0 && !is_wrapped ? level::unary : level::primary};
            },
            [&](flat_bool_literal const& l) { result = {.text = l.value ? "true" : "false"}; },
            [&](flat_enum_value const& v)
            { result = {.text = p.enums[p.enum_of_type[index_of(x.type)]].case_names[v.case_index]}; },
            [&](flat_local_ref const& l) { result = {.text = p.locals[index_of(l.local)]}; },
            [&](flat_binding_member const& b)
            {
                // A buffer is a global of its own; every other member is a field of the inline constants block.
                if (auto const found = buffer_of(p, b.binding, b.member); found >= 0)
                {
                    result = {.text = d.buffer_reference(p.buffers[found])};
                    return;
                }
                auto const& constants = p.constants.value();
                result = {.text = cc::format("{}.{}", constants.name, constants.members[b.member].name)};
            },
            [&](flat_buffer_element const& b)
            {
                auto const buffer = wrapped(expr(b.buffer), level::primary);
                result = {.text = cc::format("{}[{}]", buffer, expr(b.index).text), .binds = level::primary};
            },
            [&](flat_member const& member)
            {
                auto const object_type = p.e.at(member.object).type;
                auto object = wrapped(expr(member.object), level::primary);
                // A builtin's fields are spelled alike everywhere: x, y, z, w.
                auto const name
                    = is_builtin_type(p.m, object_type)
                        ? cc::string_view(p.m.at(p.m.at(object_type).members)[member.member].name)
                        : cc::string_view(p.structs[p.struct_of_type[index_of(object_type)]].members[member.member].name);
                result = {.text = cc::format("{}.{}", object, name)};
            },
            [&](flat_construct const&) { result = construct(x, is_broken); },
            [&](flat_call const& c) { result = call(c); }, [&](flat_not const& n)
            { result = {.text = cc::format("!{}", wrapped(expr(n.operand), level::primary)), .binds = level::unary}; },
            [&](flat_and const& a) { result = logical("&&", level::logical_and, expr(a.lhs), expr(a.rhs)); },
            [&](flat_or const& o) { result = logical("||", level::logical_or, expr(o.lhs), expr(o.rhs)); },
            // never in a core tree, which is all that reaches a writer
            [&](flat_block const&) {});
        return result;
    }

    void body(ast::range_of<flat_stmt_id> range)
    {
        for (auto const id : p.e.at(range))
            statement(p.e.at(id));
    }

    /// `is_chained` writes `else if` onto the `if` before it.
    void branch(flat_if const& s, bool is_chained)
    {
        auto const head = condition_text(is_chained ? "else if" : "if", expr(s.condition).text);
        if (is_chained)
            reopen(head);
        else
            open(head);
        body(s.then_body);

        auto const else_body = p.e.at(s.else_body);
        if (else_body.empty())
            return close();
        // `else if` reads better than an `if` nested in an `else`, as long as the condition writes no line of its own
        auto const* const nested = else_body.size() == 1 ? p.e.at(else_body[0]).node.try_as<flat_if>() : nullptr;
        if (nested != nullptr && !writes_lines(nested->condition))
            return branch(*nested, true);
        reopen("else");
        body(s.else_body);
        close();
    }

    void loop_while(flat_while const& s)
    {
        if (!writes_lines(s.condition))
        {
            open(condition_text("while", expr(s.condition).text));
            body(s.body);
            return close();
        }
        // The lines the condition writes must run before every test, so the test moves into the loop.
        open(d.is_c_like() ? "while (true)" : "loop");
        auto const condition = cc::format("!{}", wrapped(expr(s.condition), level::primary));
        open(condition_text("if", condition));
        line("break;");
        close();
        body(s.body);
        close();
    }

    void once(flat_once const& s)
    {
        if (d.is_c_like())
        {
            open("do");
            body(s.body);
            return close(" while (false);");
        }
        // WGSL has no `do`: a loop that ends in a `break` runs once, and the `break` is left out where no path reaches it
        open("loop");
        body(s.body);
        auto const statements = p.e.at(s.body);
        auto const ends_in_exit
            = !statements.empty()
           && (p.e.at(statements.back()).node.is<flat_break>() || p.e.at(statements.back()).node.is<flat_return>());
        if (!ends_in_exit)
            line("break;");
        close();
    }

    void declare(local_id id, flat_expr_id value, bool is_mut)
    {
        auto const& local = p.e.at(id);
        auto const name = cc::string_view(p.locals[index_of(id)]);
        if (is_valid(value) && needs_member_assignment(p.e.at(value)))
        {
            build_struct(p.e.at(value), name);
            return;
        }
        auto const text = is_valid(value) ? expr(value, true).text : cc::string();
        auto declaration = cc::string();
        d.write_local(declaration, {.name = name, .type = type_text(p, d, local.type), .value = text, .is_mut = is_mut});
        line(declaration);
    }

    /// True when the arm's last statement already leaves the switch, so the C-like targets need no `break` of their own.
    [[nodiscard]] bool ends_in_exit(ast::range_of<flat_stmt_id> range) const
    {
        auto const statements = p.e.at(range);
        if (statements.empty())
            return false;
        auto const& last = p.e.at(statements.back());
        return last.node.is<flat_break>() || last.node.is<flat_return>() || last.node.is<flat_continue>();
    }

    /// One arm: its labels, its body, and the `break` that stops the C-like targets falling into the next one.
    void arm(cc::span<flat_expr_id const> patterns, ast::range_of<flat_stmt_id> range)
    {
        if (d.is_c_like())
        {
            for (auto const pattern : patterns)
                line(cc::format("case {}:", expr(pattern).text));
            if (patterns.empty())
                line("default:");
            ++depth;
            body(range);
            if (!ends_in_exit(range))
                line("break;");
            --depth;
            return;
        }
        auto labels = cc::string();
        for (auto const pattern : patterns)
        {
            if (!labels.empty())
                labels += ", ";
            labels += expr(pattern).text;
        }
        open(patterns.empty() ? cc::string("default:") : cc::format("case {}:", labels));
        body(range);
        close();
    }

    void switch_(flat_switch const& s)
    {
        open(condition_text("switch", expr(s.scrutinee).text));
        for (auto const& a : p.e.at(s.arms))
            arm(p.e.at(a.patterns), a.body);
        arm({}, s.default_body);
        close();
    }

    void statement(flat_stmt const& s)
    {
        s.node.visit([&](flat_let const& let) { declare(let.local, let.value, p.e.at(let.local).is_mut); },
                     [&](flat_var const& var) { declare(var.local, var.value, true); },
                     [&](flat_assign const& a)
                     {
                         auto const value = expr(a.value, true).text;
                         line(cc::format("{} = {};", expr(a.place).text, value));
                     },
                     // `validate` refuses a tree that holds one
                     [&](flat_print const&) {}, //
                     [&](flat_eval const& v)
                     {
                         auto text = cc::string();
                         d.write_eval(text, expr(v.value).text);
                         line(text);
                     },
                     [&](flat_if const& i) { branch(i, false); },
                     // none of the three is in a core tree
                     [&](flat_block const&) {}, //
                     [&](flat_leave const&) {}, //
                     [&](flat_case const&) {},
                     [&](flat_loop const& l)
                     {
                         open(d.is_c_like() ? "while (true)" : "loop");
                         body(l.body);
                         close();
                     },
                     [&](flat_while const& w) { loop_while(w); },
                     [&](flat_for const& f)
                     {
                         auto const first = expr(f.first).text;
                         auto const end = wrapped(expr(f.end), level::additive);
                         auto head = cc::string();
                         d.write_for_head(head, p.locals[index_of(f.index)], first, end);
                         open(head);
                         body(f.body);
                         close();
                     },
                     [&](flat_continue const&) { line("continue;"); }, //
                     [&](flat_once const& o) { once(o); },             //
                     [&](flat_break const&) { line("break;"); },       //
                     [&](flat_switch const& sw) { switch_(sw); },
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

void sgl::emit::impl::write_enum_constants(cc::string& out, plan const& p, dialect const& d)
{
    for (auto const& e : p.enums)
    {
        auto const cases = p.m.at(p.m.at(e.type).cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            d.write_enum_constant(out, e.case_names[i], cases[i].value);
        if (!cases.empty())
            out += "\n";
    }
}

void sgl::emit::impl::write_buffers(cc::string& out, plan const& p, dialect const& d)
{
    // `p.buffers` is in group then slot order, so one binding's buffers are one run.
    for (auto first = isize(0); first < p.buffers.size();)
    {
        auto last = first;
        while (last < p.buffers.size() && p.buffers[last].binding == p.buffers[first].binding)
            ++last;
        d.write_buffer_group(
            out, p, cc::span<planned_buffer const>(p.buffers).subspan({.offset = first, .size = last - first}));
        first = last;
    }
}

cc::string_view sgl::emit::impl::type_text(plan const& p, dialect const& d, check::type_id type)
{
    if (auto const* const record = p.m.builtin_type_of(type))
        return record->spelled_in(d.language());
    // An enum is its cases' `int` on every target (EVAL-64); the constants of EMIT-76 are what its values read as.
    if (is_valid(type) && p.m.at(type).kind == check::type_kind::enumeration && p.m.builtins != nullptr)
    {
        auto const id = p.m.builtins->find_type(builtins::k_int);
        if (p.m.builtins->is_known(id))
            return p.m.builtins->at(id).spelled_in(d.language());
    }
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
