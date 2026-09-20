#pragma once

#include "../legalize/flat-test-support.hh"

namespace sgl_test
{
/// The two edge structs every program of these tests stands between; `p.a` and `p.b` are what a run is given.
constexpr auto frag_edges = cc::string_view("struct frag:\n"
                                            "    a: float\n"
                                            "    b: float\n"
                                            "\n"
                                            "@pixel struct target:\n"
                                            "    color: float4\n"
                                            "\n");

/// What ends every body here: the value under test as a grey.
constexpr auto return_grey = cc::string_view("return {\n    color = float4(x, x, x, 1.0)\n}\n");

/// `functions` behind the edge structs, checked against the library's prelude.
inline checked_sources check_program(cc::string_view functions)
{
    return check_sources(read_prelude(), cc::string(frag_edges) + functions);
}

/// `lines` as the body of `@pixel fun main_ps(p: frag) -> target`, behind `helpers`; `lines` is indented here.
inline cc::string entry_source(cc::string_view helpers, cc::string_view lines)
{
    auto source = cc::string(helpers);
    source += "@pixel fun main_ps(p: frag) -> target:\n";
    auto at_line_start = true;
    for (auto const ch : lines)
    {
        if (at_line_start && ch != '\n')
            source += "    ";
        source += ch;
        at_line_start = ch == '\n';
    }
    return source;
}

/// The same with `return_grey` behind the lines, so `x` is what the program computes.
inline cc::string grey_source(cc::string_view helpers, cc::string_view lines)
{
    return entry_source(helpers, cc::string(lines) + return_grey);
}

/// The structured body of the one entry point of a program that checks clean, as `body_dump` writes it.
inline cc::string structured_dump(cc::string_view helpers, cc::string_view lines)
{
    auto const checked = check_program(grey_source(helpers, lines));
    CHECK(reports_of(checked) == "");
    if (checked.module.entry_points.size() != 1)
        return "<no entry point>\n";
    return body_dump(checked.module, checked.module.entry_points[0]);
}

/// The same body once it is core.
inline cc::string core_dump(cc::string_view helpers, cc::string_view lines)
{
    auto const checked = check_program(grey_source(helpers, lines));
    CHECK(reports_of(checked) == "");
    if (checked.module.entry_points.size() != 1)
        return "<no entry point>\n";
    auto const& m = checked.module;
    return body_dump(m, sgl::check::legalize(m, m.entry_points[0]));
}
} // namespace sgl_test
