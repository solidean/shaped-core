#include "registry.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
constexpr cc::string_view k_header
    = "// GENERATED FILE - DO NOT EDIT.\n"
      "//\n"
      "// The builtin half of SGL's prelude, written out from the C++ builtin registry:\n"
      "//     libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/\n"
      "// The compiler never opens this file: it generates the same text in memory.\n"
      "// This copy is committed so that the prelude can be read, reviewed and diffed.\n"
      "//\n"
      "// A hand edit FAILS `uv run dev.py check` (the `sgl-prelude` step) and the library's own tests.\n"
      "// To change a builtin, edit its record in the registry and regenerate this file:\n"
      "//     uv run dev.py check sgl-prelude --fix\n"
      "// which runs\n"
      "//     uv run dev.py run sgl -- prelude --write libs/graphics/shaped-graphics-language/prelude/builtins.sgl\n"
      "//\n"
      "// Every declaration is @builtin: the registry record behind it says how it evaluates and how each target "
      "spells it.\n"
      "// Prelude code that needs no C++ is hand-written, next to this file in core.sgl.\n";

/// The name an expression in a type position is, when it is a bare name.
cc::string_view type_name_at(parsed_file const& file, ast::file_ast const& ast, ast::expr_id type)
{
    if (!ast::is_valid(type))
        return {};
    auto const* const n = ast.at(type).node.try_as<ast::name>();
    return n != nullptr ? file.text_of(n->where) : cc::string_view();
}
} // namespace

cc::string_view type_record::spelled_in(language l) const
{
    switch (l)
    {
    case language::hlsl:
        return hlsl;
    case language::wgsl:
        return wgsl;
    case language::msl:
        return msl;
    }
    return {};
}

cc::string_view function_record::called_in(language l) const
{
    auto const& own = l == language::hlsl ? write.hlsl : (l == language::wgsl ? write.wgsl : write.msl);
    if (!own.empty())
        return own;
    return write.text.empty() ? cc::string_view(name) : cc::string_view(write.text);
}

builtin_type_id registry::add(type_record record)
{
    items.push_back({.kind = registry_item::kind_t::type, .index = i32(types.size())});
    types.push_back(cc::move(record));
    return builtin_type_id(types.size() - 1);
}

builtin_id registry::add(function_record record)
{
    items.push_back({.kind = registry_item::kind_t::function, .index = i32(functions.size())});
    functions.push_back(cc::move(record));
    return builtin_id(functions.size() - 1);
}

void registry::add_comment(cc::string_view text)
{
    items.push_back({.kind = registry_item::kind_t::comment, .comment = text});
}

cc::string registry::prelude_text() const
{
    auto out = cc::string(k_header);
    auto const doc = [&](cc::string_view lines)
    {
        if (!lines.empty())
            out.appendf("{}\n", lines);
    };
    auto previous = registry_item::kind_t::comment;
    for (auto const& item : items)
    {
        switch (item.kind)
        {
        case registry_item::kind_t::comment:
            out.appendf("\n{}\n", item.comment);
            break;
        case registry_item::kind_t::type:
            // a struct with a block needs the empty line, and one without reads better with it
            if (previous != registry_item::kind_t::comment)
                out += "\n";
            doc(types[item.index].doc);
            out.appendf("@builtin {}\n", types[item.index].declaration);
            break;
        case registry_item::kind_t::function:
            if (previous == registry_item::kind_t::type)
                out += "\n";
            doc(functions[item.index].doc);
            out.appendf("@builtin {}\n", functions[item.index].signature);
            break;
        }
        previous = item.kind;
    }
    return out;
}

void registry::finalize()
{
    auto const file = parse(prelude_text());
    auto const ast = ast::build(file);
    CC_ASSERT(file.diagnostics.empty() && ast.diagnostics.empty(), "a builtin record whose SGL text does not parse");

    auto const declarations = ast.at(ast.declarations);
    auto next = isize(0);
    // types first, so a signature may name a type registered behind it
    for (auto const& item : items)
    {
        if (item.kind == registry_item::kind_t::comment)
            continue;
        CC_ASSERT(next < declarations.size(), "a builtin record that is no declaration");
        auto const& d = ast.at(declarations[next++]);
        if (item.kind != registry_item::kind_t::type)
            continue;
        auto const* const s = d.node.try_as<ast::struct_decl>();
        CC_ASSERT(s != nullptr, "a builtin type record that is no struct");
        types[item.index].name = file.text_of(s->name);
    }
    CC_ASSERT(next == declarations.size(), "a builtin record that is more than one declaration");

    next = 0;
    for (auto const& item : items)
    {
        if (item.kind == registry_item::kind_t::comment)
            continue;
        auto const& d = ast.at(declarations[next++]);
        if (item.kind != registry_item::kind_t::function)
            continue;
        auto const* const f = d.node.try_as<ast::fun_decl>();
        CC_ASSERT(f != nullptr, "a builtin function record that is no fun");
        auto& record = functions[item.index];
        record.name = file.text_of(f->name);
        record.parameters.clear();
        for (auto const& p : ast.at(f->parameters))
        {
            auto const type = find_type(type_name_at(file, ast, p.type));
            CC_ASSERT(is_valid(type), "a builtin signature that names a type nobody registered");
            record.parameters.push_back(type);
        }
        record.result = find_type(type_name_at(file, ast, f->return_type));
        CC_ASSERT(is_valid(record.result), "a builtin signature whose result is no registered type");
        CC_ASSERT(record.evaluate != nullptr, "a builtin function without an evaluator");
        CC_ASSERT(record.write.kind != spelling_kind::custom || record.write.custom != nullptr, "a custom spelling "
                                                                                                "without a writer");
    }
}

builtin_type_id registry::find_type(cc::string_view name) const
{
    for (auto i = isize(0); i < types.size(); ++i)
        if (!name.empty() && types[i].name == name)
            return builtin_type_id(i);
    return builtin_type_id::none;
}

builtin_id registry::find_function(cc::string_view name, cc::span<builtin_type_id const> parameters) const
{
    for (auto i = isize(0); i < functions.size(); ++i)
    {
        auto const& f = functions[i];
        if (f.name != name || f.parameters.size() != parameters.size())
            continue;
        auto is_match = true;
        for (auto k = isize(0); k < parameters.size(); ++k)
            is_match = is_match && f.parameters[k] == parameters[k];
        if (is_match)
            return builtin_id(i);
    }
    return builtin_id::none;
}

bool registry::has_function_named(cc::string_view name) const
{
    for (auto const& f : functions)
        if (f.name == name)
            return true;
    return false;
}

cc::string sgl::builtins::wrapped(written w, precedence needed)
{
    return w.binds < needed ? cc::format("({})", w.text) : cc::move(w.text);
}

written sgl::builtins::write_infix(cc::string_view op, precedence own, written lhs, written rhs)
{
    auto const tighter = precedence(u8(own) + 1);
    return {.text = cc::format("{} {} {}", wrapped(cc::move(lhs), own), op, wrapped(cc::move(rhs), tighter)),
            .binds = own};
}

spelling sgl::builtins::infix(cc::string_view op)
{
    auto binds = precedence::comparison;
    if (op == "+" || op == "-")
        binds = precedence::additive;
    else if (op == "*" || op == "/")
        binds = precedence::multiplicative;
    else
        CC_ASSERT(op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=",
                  "an operator no target has a level for");
    return {.kind = spelling_kind::infix, .text = op, .binds = binds};
}

registry sgl::builtins::make_registry()
{
    auto result = registry();
    register_builtins(result);
    result.finalize();
    return result;
}

registry const& sgl::builtins::default_registry()
{
    static auto const instance = make_registry();
    return instance;
}
