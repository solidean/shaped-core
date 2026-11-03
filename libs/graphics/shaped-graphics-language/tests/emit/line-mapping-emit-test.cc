#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// A helper on lines 4-6 inlined into an entry point on lines 9-11, with a branch whose braces are no statement.
constexpr cc::string_view k_scaled = "binding work:\n"
                                     "    values: mut buffer[float]\n"
                                     "\n"
                                     "fun scaled(x: float) -> float:\n"
                                     "    let y = x * 2.0\n"
                                     "    return y + 1.0\n"
                                     "\n"
                                     "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                     "    let v = work.values[id.x]\n"
                                     "    if v > 0.0 => work.values[id.x] = scaled(v)\n"
                                     "    work.values[id.x + 1] = v\n";

sgl::emitted_source compile(cc::string_view source, target t, bool line_directives, cc::string_view name = "lines.sgl")
{
    auto r = sgl::compile_to_text(
        {.source = source, .source_name = name, .entry_point = "cs", .target = t, .line_directives = line_directives});
    REQUIRE(r.has_value());
    return cc::move(r.value());
}

/// Line `n` of `text`, counted from 1.
cc::string_view line_of(cc::string_view text, i32 n)
{
    auto start = isize(0);
    for (auto i = 1; i < n; ++i)
    {
        while (start < text.size() && text[start] != '\n')
            ++start;
        ++start;
    }
    auto end = start;
    while (end < text.size() && text[end] != '\n')
        ++end;
    return text.subview({.offset = start, .size = end - start});
}

/// The source line the table maps the first emitted line holding `needle` to, and the calls it was inlined through.
sgl::emit::line_origin origin_of_text(sgl::emitted_source const& e, cc::string_view needle)
{
    for (auto const& o : e.lines)
        if (line_of(e.text, o.emitted).contains(needle))
            return o;
    CHECK(false).dump("no line of the table holds", needle);
    return {};
}
} // namespace

TEST("sgl emit - each line of the body maps to the source line it writes, on every target")
{
    for (auto const t : sgl::emit::all_targets())
    {
        auto const e = compile(k_scaled, t, false);
        CHECK(!e.lines.empty()).dump("target", sgl::emit::to_string(t));
        REQUIRE(e.line_files.size() > e.lines[0].at.file);
        CHECK(e.line_files[e.lines[0].at.file] == "lines.sgl");

        auto const v = origin_of_text(e, "values[id.x + 1]");
        CHECK(v.at.line == 11).dump("target", sgl::emit::to_string(t));
        CHECK(v.inlined_from.empty());
    }
}

TEST("sgl emit - an inlined body maps to its own line, and names the call it was inlined through")
{
    auto const e = compile(k_scaled, target::hlsl_dx12, false);
    auto const y = origin_of_text(e, "* 2.0");
    CHECK(y.at.line == 5);
    REQUIRE(y.inlined_from.size() == 1);
    CHECK(y.inlined_from[0].line == 10);
    CHECK(y.inlined_from[0].file == y.at.file);
}

TEST("sgl emit - `#line` names the SGL line in HLSL and MSL, once per statement whose line the compiler would miss")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
    {
        auto const text = compile(k_scaled, t, true).text;
        CHECK(text.contains("#line 9 \"lines.sgl\"\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains("#line 5 \"lines.sgl\"\n"));
        CHECK(text.contains("// inlined from lines.sgl:10\n"));
    }

    // a brace is no statement of its own, so a branch takes no directive for its braces
    auto const hlsl = compile(k_scaled, target::hlsl_dx12, true).text;
    auto directives = 0;
    for (auto i = isize(0); i + 6 < hlsl.size(); ++i)
        directives += hlsl.subview({.offset = i, .size = 6}) == "#line " ? 1 : 0;
    CHECK(directives <= 6).dump("text", hlsl);
}

TEST("sgl emit - the table counts the directive lines, so its lines are the lines a compiler reports")
{
    auto const e = compile(k_scaled, target::hlsl_dx12, true);
    for (auto const& o : e.lines)
        CHECK(!line_of(e.text, o.emitted).starts_with("#line")).dump("line", o.emitted);
    CHECK(origin_of_text(e, "values[id.x + 1]").at.line == 11);
}

TEST("sgl emit - WGSL has no `#line`, and a compile that asks for none writes none")
{
    CHECK(!compile(k_scaled, target::wgsl, true).text.contains("#line"));
    CHECK(!compile(k_scaled, target::hlsl_dx12, false).text.contains("#line"));
    CHECK(!compile(k_scaled, target::hlsl_dx12, false).text.contains("// inlined from"));
}

TEST("sgl emit - a `#line` name is a string literal, so a Windows path's backslashes are escaped")
{
    auto const text = compile(k_scaled, target::hlsl_dx12, true, "C:\\shaders\\lines.sgl").text;
    CHECK(text.contains("#line 9 \"C:\\\\shaders\\\\lines.sgl\"\n"));
    // a comment names the file alone
    CHECK(text.contains("// inlined from lines.sgl:10\n"));
}
