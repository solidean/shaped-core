#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/ast/dump.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl_test
{
using namespace cc::primitive_defines;

/// The AST dump of `source` without its last newline.
/// ` !! ` and the AST diagnostics follow when there are any, and ` !!syntax ` and the syntactic ones after that,
/// so a test that expects a clean dump fails on either.
inline cc::string ast_of(cc::string_view source)
{
    auto const file = sgl::parse(source);
    auto const ast = sgl::ast::build(file);
    auto dump = sgl::ast::dump(file, ast);
    if (dump.ends_with("\n"))
        dump.resize_down_to(dump.size() - 1);

    auto const diagnostics = sgl::ast::dump_diagnostics(ast);
    if (!diagnostics.empty())
        dump += " !! " + diagnostics;
    auto const syntax = sgl::dump_diagnostics(file);
    if (!syntax.empty())
        dump += " !!syntax " + syntax;
    return dump;
}

/// The dump of one expression, read as the value of `const r = …`.
/// Diagnostic offsets count from the start of that line, so the expression starts at 10.
inline cc::string expr_of(cc::string_view expression)
{
    auto const dump = ast_of(cc::string("const r = ") + expression);
    auto const prefix = cc::string_view("(const r = ");
    CC_ASSERT(dump.starts_with(prefix), "the wrapper must stay a const declaration");

    auto const end = dump.contains(" !!") ? dump.find(" !!") : dump.size();
    auto result = cc::string(cc::string_view(dump).subview({.offset = prefix.size(), .size = end - 1 - prefix.size()}));
    result += cc::string_view(dump).subview({.offset = end, .size = dump.size() - end});
    return result;
}

/// The dump of the statements of a function body; `lines` is indented here, one level.
/// Every statement line comes back without the indentation of the body it stood in.
inline cc::string body_of(cc::string_view lines)
{
    auto source = cc::string("fun f():\n");
    auto at_line_start = true;
    for (auto const c : lines)
    {
        if (at_line_start && c != '\n')
            source += "    ";
        source += c;
        at_line_start = c == '\n';
    }

    auto const dump = ast_of(source);
    auto const prefix = cc::string_view("(fun f (params)");
    CC_ASSERT(dump.starts_with(prefix), "the wrapper must stay a function");

    auto const end = dump.contains(" !!") ? dump.find(" !!") : dump.size();
    auto result = cc::string();
    auto const body = cc::string_view(dump).subview({.offset = prefix.size(), .size = end - 1 - prefix.size()});
    for (auto i = isize(0); i < body.size(); ++i)
    {
        result += body[i];
        if (body[i] == '\n')
            i += 2;
    }
    if (result.starts_with("\n"))
        result = cc::string(cc::string_view(result).subview({.offset = 1, .size = result.size() - 1}));
    result += cc::string_view(dump).subview({.offset = end, .size = dump.size() - end});
    return result;
}
} // namespace sgl_test
