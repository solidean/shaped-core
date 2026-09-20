#include "../emit/emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
cc::string cube_source()
{
    return read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl");
}

cc::string error_of(sgl::text_request const& request)
{
    auto const r = sgl::compile_to_text(request);
    REQUIRE(r.has_error());
    return r.error();
}
} // namespace

TEST("sgl driver - the prelude is the generated builtins and the hand-written core, and both match their files")
{
    auto const files = sgl::prelude_files();
    REQUIRE(files.size() == 2);
    CHECK(files[0].name == "builtins.sgl");
    CHECK(files[1].name == "core.sgl");

    // The committed builtins.sgl is what the registry generates, byte for byte: `uv run dev.py check sgl-prelude --fix` rewrites it.
    // That is also what makes a diagnostic's line and column right in the committed file, which the compiler never opens.
    CHECK(files[0].source == read_text(cc::string(SGL_PRELUDE_DIR) + "/builtins.sgl"));
    // core.sgl is embedded when CMake configures, and editing it re-runs the configure.
    CHECK(files[1].source == read_text(cc::string(SGL_PRELUDE_DIR) + "/core.sgl"));
}

TEST("sgl driver - a line and a column are 1-based, and end of file is a place")
{
    auto const source = cc::string_view("ab\ncd\r\nef");
    auto const at = [](i32 line, i32 column) { return sgl::line_column{.line = line, .column = column}; };
    CHECK(sgl::line_column_of(source, 0) == at(1, 1));
    CHECK(sgl::line_column_of(source, 2) == at(1, 3));
    CHECK(sgl::line_column_of(source, 3) == at(2, 1));
    CHECK(sgl::line_column_of(source, 7) == at(3, 1));
    CHECK(sgl::line_column_of(source, 9) == at(3, 3));
    CHECK(sgl::line_column_of(source, 100) == at(3, 3));
}

TEST("sgl driver - a diagnostic is one line with its place, its level and its kind")
{
    auto const d = sgl::diagnostic{.kind = sgl::diagnostic_kind::unknown_name,
                                   .level = sgl::severity::normal_error,
                                   .where = {.offset = 4, .length = 3}};
    CHECK(sgl::format_diagnostic("a.sgl", "ab\ncd", d, "foo") == "a.sgl:2:2: error: unknown-name: foo");
    CHECK(sgl::format_diagnostic("a.sgl", "ab\ncd", d) == "a.sgl:2:2: error: unknown-name");
}

TEST("sgl driver - an entry point is found by its name, and the text is the emitter's")
{
    auto const source = cube_source();
    auto const checked = check_sources(read_prelude(), source);
    for (auto const t : sgl::emit::all_targets())
    {
        auto const vs = sgl::compile_to_text({.source = source, .entry_point = "main_vs", .target = t});
        auto const ps = sgl::compile_to_text(
            {.source = source, .entry_point = "main_ps", .stage = sgl::check::stage::pixel, .target = t});
        REQUIRE(vs.has_value());
        REQUIRE(ps.has_value());
        CHECK(vs.value().text == sgl::emit::emit(checked.module, 0, t).text);
        CHECK(ps.value().text == sgl::emit::emit(checked.module, 1, t).text);
    }
}

TEST("sgl driver - a missing entry point names the ones the source holds")
{
    auto const source = cube_source();
    CHECK(error_of({.source = source, .source_name = "cube.sgl", .entry_point = "main_cs", .target = target::wgsl})
          == "cube.sgl: error: no entry point named 'main_cs' (the source holds: vertex 'main_vs', pixel 'main_ps')\n");
}

TEST("sgl driver - an entry point of another stage is an error")
{
    auto const source = cube_source();
    CHECK(error_of({.source = source,
                    .source_name = "cube.sgl",
                    .entry_point = "main_ps",
                    .stage = sgl::check::stage::vertex,
                    .target = target::wgsl})
          == "cube.sgl: error: entry point 'main_ps' is a pixel entry point, and a vertex one was asked for\n");
}

TEST("sgl driver - a broken source reports where and what, and gives no text")
{
    auto const source = cc::string_view("@pixel struct target:\n"
                                        "    color: float4\n"
                                        "\n"
                                        "struct pixel_input:\n"
                                        "    @position position: hpos4\n"
                                        "\n"
                                        "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                        "    return {\n"
                                        "        color = float4(missing, 0.0, 0.0, 1.0)\n"
                                        "    }\n");
    CHECK(error_of({.source = source, .source_name = "broken.sgl", .entry_point = "main_ps", .target = target::hlsl_dx12})
          == "broken.sgl:9:24: error: unknown-name: missing\n");
}
