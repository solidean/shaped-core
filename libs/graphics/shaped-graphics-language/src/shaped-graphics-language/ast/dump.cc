#include "dump.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::ast;

struct dumper
{
    parsed_file const& file;
    file_ast const& ast;
    cc::string out;

    void new_line(int depth)
    {
        out += "\n";
        for (auto i = 0; i < depth; ++i)
            out += "  ";
    }

    void name_or_missing(source_span where)
    {
        out += where.empty() ? cc::string_view("<missing>") : file.text_of(where);
    }

    void token_text(token_id token)
    {
        if (is_valid(token))
            out += file.text_of(file.at(token).where);
    }

    void first_line_quoted(form_id form)
    {
        auto text = file.text_of(file.at(form).where);
        for (auto i = isize(0); i < text.size(); ++i)
            if (text[i] == '\n' || text[i] == '\r')
            {
                text = text.subview({.offset = 0, .size = i});
                break;
            }
        out += "\"";
        out += text;
        out += "\"";
    }

    void attributes(range_of<attribute> range)
    {
        if (range.empty())
            return;
        out += "{";
        auto is_first = true;
        for (auto const& a : ast.at(range))
        {
            if (!is_first)
                out += " ";
            is_first = false;
            out += "@";
            out += file.text_of(a.name);
            if (!is_valid(a.list))
                continue;
            out += "(";
            auto is_first_argument = true;
            for (auto const& argument : ast.at(a.arguments))
            {
                if (!is_first_argument)
                    out += " ";
                is_first_argument = false;
                dump_argument(argument, 0);
            }
            out += ")";
        }
        out += "}";
    }

    // ---- parts ------------------------------------------------------------------------------------------------

    void dump_argument(argument const& a, int depth)
    {
        if (a.is_splat)
            out += "..";
        if (!a.name.empty())
        {
            out += file.text_of(a.name);
            out += "=";
        }
        if (a.is_shorthand)
            out += "<shorthand>";
        else
            dump_expr(a.value, depth);
        attributes(a.attributes);
    }

    void dump_arguments(range_of<argument> range, int depth)
    {
        for (auto const& a : ast.at(range))
        {
            out += " ";
            dump_argument(a, depth);
        }
    }

    void dump_field(field const& f, int depth)
    {
        out += "(field";
        attributes(f.attributes);
        if (f.is_mut)
            out += " mut";
        if (!f.name.empty())
        {
            out += " ";
            out += file.text_of(f.name);
        }
        if (is_valid(f.type))
        {
            out += " : ";
            dump_expr(f.type, depth);
        }
        if (is_valid(f.default_value))
        {
            out += " = ";
            dump_expr(f.default_value, depth);
        }
        out += ")";
    }

    void dump_fields(cc::string_view tag, range_of<field> range, int depth)
    {
        out += "(";
        out += tag;
        for (auto const& f : ast.at(range))
        {
            out += " ";
            dump_field(f, depth);
        }
        out += ")";
    }

    void dump_body(body const& b, int depth)
    {
        if (b.kind == body_kind::arrow)
        {
            out += " => ";
            if (is_valid(b.value))
                dump_expr(b.value, depth);
            for (auto const s : ast.at(b.statements))
                dump_stmt(s, depth);
        }
        else if (b.kind == body_kind::block)
        {
            for (auto const s : ast.at(b.statements))
            {
                new_line(depth + 1);
                dump_stmt(s, depth + 1);
            }
        }
    }

    // ---- expressions ------------------------------------------------------------------------------------------

    void dump_expr(expr_id id, int depth)
    {
        if (!is_valid(id))
        {
            out += "<none>";
            return;
        }
        auto const& e = ast.at(id);
        auto const unary = [&](cc::string_view tag, expr_id value)
        {
            out += "(";
            out += tag;
            if (is_valid(value))
            {
                out += " ";
                dump_expr(value, depth);
            }
            out += ")";
        };
        auto const typed = [&](cc::string_view tag, expr_id value, expr_id type)
        {
            out += "(";
            out += tag;
            out += " ";
            dump_expr(value, depth);
            out += " : ";
            dump_expr(type, depth);
            out += ")";
        };
        auto const list = [&](cc::string_view tag, range_of<argument> elements)
        {
            out += "(";
            out += tag;
            dump_arguments(elements, depth);
            out += ")";
        };

        e.node.visit(
            [&](invalid_expr const&)
            {
                out += "(invalid ";
                first_line_quoted(e.form);
                out += ")";
            },
            [&](literal const& n)
            {
                out += n.kind == literal_kind::number ? "num:" : n.kind == literal_kind::quoted ? "str:" : "hash:";
                out += file.text_of(file.at(e.form).where);
            },
            [&](name const& n) { out += file.text_of(n.where); }, [&](self_ref const&) { out += "self"; },
            [&](wildcard const&) { out += "_"; },
            [&](leading_dot const& n)
            {
                out += ".";
                out += file.text_of(n.name);
            },
            [&](member const& n)
            {
                out += "(member ";
                dump_expr(n.object, depth);
                out += " ";
                out += file.text_of(n.name);
                out += ")";
            },
            [&](index const& n)
            {
                out += "(index ";
                dump_expr(n.object, depth);
                dump_arguments(n.arguments, depth);
                out += ")";
            },
            [&](call const& n)
            {
                switch (n.spelling)
                {
                case call_spelling::paren:
                    out += "(call:paren ";
                    break;
                case call_spelling::juxtaposition:
                    out += "(call:juxt ";
                    break;
                case call_spelling::infix:
                    out += n.is_short_circuit ? "(call:infix:short-circuit " : "(call:infix ";
                    break;
                case call_spelling::prefix:
                    out += "(call:prefix ";
                    break;
                }
                if (is_valid(n.callee))
                    dump_expr(n.callee, depth);
                else
                    token_text(n.op);
                dump_arguments(n.arguments, depth);
                out += ")";
            },
            [&](tuple const& n) { list("tuple", n.elements); }, [&](array const& n) { list("array", n.elements); },
            [&](object const& n) { list("object", n.elements); },
            [&](comparison_chain const& n)
            {
                out += "(chain";
                auto const operands = ast.at(n.operands);
                auto const operators = ast.at(n.operators);
                for (auto i = isize(0); i < operands.size(); ++i)
                {
                    out += " ";
                    dump_expr(operands[i], depth);
                    if (i < operators.size())
                    {
                        out += " ";
                        token_text(operators[i]);
                    }
                }
                out += ")";
            },
            [&](cast const& n) { typed("cast", n.value, n.type); },
            [&](membership const& n)
            {
                out += "(in ";
                dump_expr(n.value, depth);
                out += " ";
                dump_expr(n.container, depth);
                out += ")";
            },
            [&](ascription const& n) { typed("ascribe", n.value, n.type); },
            [&](range const& n)
            {
                out += "(range ";
                token_text(n.op);
                out += " ";
                dump_expr(n.first, depth);
                out += " ";
                dump_expr(n.last, depth);
                out += ")";
            },
            [&](lambda const& n)
            {
                out += n.spelling == lambda_spelling::fun ? "(lambda:fun " : "(lambda ";
                if (!n.type_parameters.empty())
                {
                    dump_fields("type-params", n.type_parameters, depth);
                    out += " ";
                }
                dump_fields("params", n.parameters, depth);
                if (!n.bindings.empty())
                {
                    out += " (uses";
                    dump_arguments(n.bindings, depth);
                    out += ")";
                }
                if (is_valid(n.return_type))
                {
                    out += " -> ";
                    dump_expr(n.return_type, depth);
                }
                dump_body(n.body, depth);
                out += ")";
            },
            [&](case_expr const& n)
            {
                out += "(case ";
                dump_expr(n.value, depth);
                for (auto const& arm : ast.at(n.arms))
                {
                    new_line(depth + 1);
                    out += "(arm ";
                    if (is_valid(arm.pattern))
                        dump_expr(arm.pattern, depth + 1);
                    else
                        out += "<missing>";
                    dump_body(arm.result, depth + 1);
                    out += ")";
                }
                out += ")";
            },
            [&](loop_expr const& n)
            {
                out += "(loop";
                dump_body(n.body, depth);
                out += ")";
            },
            [&](return_expr const& n) { unary("return", n.value); }, [&](yield_expr const& n)
            { unary("yield", n.value); }, [&](break_expr const& n) { unary("break", n.value); }, [&](continue_expr const&)
            { out += "(continue)"; }, [&](struct_type const& n) { dump_fields("struct-type", n.fields, depth); },
            [&](function_type const& n)
            {
                out += "(function-type ";
                dump_fields("params", n.parameters, depth);
                out += " -> ";
                dump_expr(n.result, depth);
                out += ")";
            },
            [&](with_bindings const& n)
            {
                out += "(with-bindings ";
                dump_expr(n.target, depth);
                dump_arguments(n.bindings, depth);
                out += ")";
            });
        attributes(e.attributes);
    }

    // ---- statements -------------------------------------------------------------------------------------------

    void dump_stmt(stmt_id id, int depth)
    {
        auto const& s = ast.at(id);
        auto const open = [&](cc::string_view tag)
        {
            out += "(";
            out += tag;
            attributes(s.attributes);
        };

        s.node.visit(
            [&](invalid_stmt const&)
            {
                open("invalid-stmt");
                out += " ";
                first_line_quoted(s.form);
                out += ")";
            },
            [&](let_stmt const& n)
            {
                open("let");
                if (n.is_mut)
                    out += " mut";
                out += " ";
                dump_expr(n.pattern, depth);
                if (is_valid(n.type))
                {
                    out += " : ";
                    dump_expr(n.type, depth);
                }
                if (is_valid(n.value))
                {
                    out += " = ";
                    dump_expr(n.value, depth);
                }
                out += ")";
            },
            [&](assign_stmt const& n)
            {
                open("assign");
                out += " ";
                token_text(n.op);
                out += " ";
                dump_expr(n.target, depth);
                out += " ";
                dump_expr(n.value, depth);
                out += ")";
            },
            [&](if_stmt const& n)
            {
                open("if");
                for (auto const& branch : ast.at(n.branches))
                {
                    new_line(depth + 1);
                    if (is_valid(branch.condition))
                    {
                        out += "(branch ";
                        dump_expr(branch.condition, depth + 1);
                    }
                    else
                        out += "(else";
                    dump_body(branch.then, depth + 1);
                    out += ")";
                }
                out += ")";
            },
            [&](for_stmt const& n)
            {
                open("for");
                out += " ";
                dump_expr(n.variable, depth);
                if (is_valid(n.type))
                {
                    out += " : ";
                    dump_expr(n.type, depth);
                }
                out += " in ";
                dump_expr(n.iterable, depth);
                dump_body(n.body, depth);
                out += ")";
            },
            [&](while_stmt const& n)
            {
                open("while");
                out += " ";
                dump_expr(n.condition, depth);
                dump_body(n.body, depth);
                out += ")";
            },
            [&](assert_stmt const& n)
            {
                open("assert");
                out += " ";
                dump_expr(n.condition, depth);
                if (is_valid(n.message))
                {
                    out += " ";
                    dump_expr(n.message, depth);
                }
                out += ")";
            },
            [&](print_stmt const& n)
            {
                open("print");
                out += " ";
                dump_expr(n.message, depth);
                out += ")";
            },
            [&](decl_stmt const& n) { dump_decl(n.declaration, depth); },
            [&](expr_stmt const& n)
            {
                dump_expr(n.value, depth);
                attributes(s.attributes);
            });
    }

    // ---- declarations -----------------------------------------------------------------------------------------

    void dump_members(range_of<decl_id> members, int depth)
    {
        for (auto const m : ast.at(members))
        {
            new_line(depth + 1);
            dump_decl(m, depth + 1);
        }
    }

    void dump_decl(decl_id id, int depth)
    {
        auto const& d = ast.at(id);
        auto const open = [&](cc::string_view tag)
        {
            out += "(";
            out += tag;
            attributes(d.attributes);
            out += " ";
        };

        d.node.visit(
            [&](invalid_decl const&)
            {
                open("invalid-decl");
                first_line_quoted(d.form);
            },
            [&](module_decl const& n)
            {
                open("module");
                dump_expr(n.path, depth);
            },
            [&](use_decl const& n)
            {
                open("use");
                dump_expr(n.path, depth);
                if (!n.alias.empty())
                {
                    out += " as ";
                    out += file.text_of(n.alias);
                }
            },
            [&](fun_decl const& n)
            {
                open("fun");
                name_or_missing(n.name);
                if (!n.type_parameters.empty())
                {
                    out += " ";
                    dump_fields("type-params", n.type_parameters, depth);
                }
                out += " ";
                dump_fields("params", n.parameters, depth);
                if (!n.bindings.empty())
                {
                    out += " (uses";
                    dump_arguments(n.bindings, depth);
                    out += ")";
                }
                if (is_valid(n.return_type))
                {
                    out += " -> ";
                    dump_expr(n.return_type, depth);
                }
                dump_body(n.body, depth);
            },
            [&](struct_decl const& n)
            {
                open(n.is_opaque ? "struct:opaque" : "struct");
                name_or_missing(n.name);
                dump_members(n.members, depth);
            },
            [&](enum_decl const& n)
            {
                open("enum");
                name_or_missing(n.name);
                dump_members(n.members, depth);
            },
            [&](type_decl const& n)
            {
                open("type");
                name_or_missing(n.name);
                out += " : ";
                dump_expr(n.value, depth);
            },
            [&](const_decl const& n)
            {
                open("const");
                name_or_missing(n.name);
                if (is_valid(n.type))
                {
                    out += " : ";
                    dump_expr(n.type, depth);
                }
                out += " = ";
                dump_expr(n.value, depth);
            },
            [&](binding_decl const& n)
            {
                open("binding");
                name_or_missing(n.name);
                if (is_valid(n.composition))
                {
                    out += " = ";
                    dump_expr(n.composition, depth);
                }
                dump_members(n.members, depth);
            },
            [&](sampler_decl const& n)
            {
                open("sampler");
                name_or_missing(n.name);
                for (auto const& setting : ast.at(n.settings))
                {
                    new_line(depth + 1);
                    dump_argument(setting, depth + 1);
                }
            },
            [&](notation_decl const& n)
            {
                open("notation");
                dump_expr(n.pattern, depth);
                out += " => ";
                dump_expr(n.replacement, depth);
            },
            [&](field_decl const& n)
            {
                // A field closes itself.
                dump_field(ast.at(n.field), depth);
            },
            [&](property_decl const& n)
            {
                open("property");
                name_or_missing(n.name);
                dump_body(n.body, depth);
            },
            [&](enum_case_decl const& n)
            {
                open("case");
                name_or_missing(n.name);
                if (is_valid(n.value))
                {
                    out += " = ";
                    dump_expr(n.value, depth);
                }
            });
        if (!d.node.is<field_decl>())
            out += ")";
    }
};
} // namespace

cc::string sgl::ast::dump(parsed_file const& file, file_ast const& ast)
{
    auto d = dumper{.file = file, .ast = ast, .out = {}};
    for (auto const id : ast.at(ast.declarations))
    {
        d.dump_decl(id, 0);
        d.out += "\n";
    }
    return cc::move(d.out);
}

cc::string sgl::ast::dump_diagnostics(file_ast const& ast)
{
    auto out = cc::string();
    for (auto const& d : ast.diagnostics)
        out.appendf("{} @{}+{}\n", to_string(d.kind), d.where.offset, d.where.length);
    return out;
}
