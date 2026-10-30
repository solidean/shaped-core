#include "../check/check-test-support.hh"

#include <clean-core/string/uri.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/driver/test_source.hh>

TEST("sgl driver - the library's own prelude files are recognized by their path, and a user's of the same name is not")
{
    auto const dir = cc::string(sgl::impl::prelude_directory());
    CHECK(sgl::prelude_file_of(dir + "/builtins.sgl") == 0);
    CHECK(sgl::prelude_file_of(dir + "/core.sgl") == 1);
    auto const uri = cc::string(dir.starts_with("/") ? "file://" : "file:///")
                   + cc::percent_encode(dir + "/core.sgl", cc::uri_component::path);
    CHECK(sgl::prelude_file_of(uri) == 1);
    auto backslashed = dir + "/core.sgl";
    for (auto i = sgl::isize(0); i < backslashed.size(); ++i)
        backslashed[i] = backslashed[i] == '/' ? '\\' : backslashed[i];
    CHECK(sgl::prelude_file_of(backslashed) == 1);

    CHECK(sgl::prelude_file_of("prelude/core.sgl") == -1);
    CHECK(sgl::prelude_file_of("shaders/prelude/core.sgl") == -1);
    CHECK(sgl::prelude_file_of("/home/me/shaders/prelude/core.sgl") == -1);
    CHECK(sgl::prelude_file_of("file:///c%3A/src/shaders/prelude/core.sgl") == -1);
    CHECK(sgl::prelude_file_of("my_prelude/core.sgl") == -1);
    CHECK(sgl::prelude_file_of(dir + "/my_core.sgl") == -1);
    CHECK(sgl::prelude_file_of(dir + "/../core.sgl") == -1);
}

TEST("sgl driver - a file of the prelude is checked in its own place, where it declares each name once")
{
    auto const dir = cc::string(sgl::impl::prelude_directory());
    auto const builtins = sgl::prelude_files()[0].source;
    CHECK(sgl::test_source(builtins, dir + "/builtins.sgl").errors == "");
    // behind the prelude, each builtin type is declared a second time, and so it is in a user's prelude folder
    CHECK(sgl::test_source(builtins, "sgl/copy/builtins.sgl").errors != "");
    CHECK(sgl::test_source(builtins, "shaders/prelude/builtins.sgl").errors != "");

    auto const core = cc::string(sgl::prelude_files()[1].source) + "fun broken() => nope\n";
    auto const core_path = dir + "/core.sgl";
    CHECK(sgl::test_source(core, core_path).errors.contains(core_path + ":"));
}

TEST("sgl driver - test_source counts the tests, reports what failed, and names the entry points")
{
    auto const tested = sgl::test_source("struct frag:\n    a: float\n@pixel struct target:\n    color: float4\n"
                                         "@pixel fun main_ps(p: frag) -> target => target(float4(p.a, p.a, p.a, 1.0))\n"
                                         "test 1 < 2\n"
                                         "test 2 < 1\n"
                                         "@expect(error = \"unknown-name\") test missing < 1\n",
                                         "t.sgl");
    CHECK(tested.test_count == 3);
    CHECK(tested.tests_expecting_diagnostics == 1);
    CHECK(tested.tests_run == 2);
    CHECK(tested.tests_passed == 1);
    CHECK(tested.errors.starts_with("t.sgl:7:1: error: test-failed: 1 of 1 checks failed\n"));
    CHECK(tested.warnings == "");
    REQUIRE(tested.entry_points.size() == 1);
    CHECK(tested.entry_points[0].name == "main_ps");

    auto const clean = sgl::test_source("test 1 < 2\n", "c.sgl");
    CHECK(clean.is_clean());
    CHECK(sgl::test_source("test:\n    1 + 2\n    true\n", "w.sgl").warnings
          == "w.sgl:2:5: warning: no-effect: a statement that computes a value and drops it\n");
}

TEST("sgl driver - a const that did not check fails what reads it silently, and never crashes")
{
    // CHK-19: a failed symbol is the error type where it is named, and the one diagnostic is its own
    cc::string_view const broken[] = {
        "const a = nope\n",       "const a: int = 1.5\n",       "const a = 7 / (0 as int)\n",
        "const a = 2147483648\n", "const a = b\nconst b = a\n",
    };
    for (auto const b : broken)
    {
        auto const source = cc::string(b) + "test a == 1\n";
        auto const tested = sgl::test_source(source, "c.sgl");
        CHECK(!tested.errors.empty()).dump("source", source);
        CHECK(!tested.errors.contains("test-failed")).dump("source", source);
        CHECK(tested.test_count == 1);
        CHECK(tested.tests_run == 0);
    }

    // reached through a callee and as a case pattern, and from an entry point
    CHECK(sgl::test_source("enum m:\n    a\n    b\nconst bad = 7 / (0 as int)\nfun f(x: m) -> int:\n    return case "
                           "x:\n"
                           "        bad => 1\n        _ => 0\ntest f(m.a) == 0\n",
                           "p.sgl")
              .errors.contains("constant-without-value"));
    auto const entry = sgl::compile_to_text({.source = "const a = nope\nstruct frag:\n    x: float\n@pixel struct "
                                                       "target:\n    color: float4\n"
                                                       "@pixel fun main_ps(p: frag) -> target:\n    let w = a\n    "
                                                       "return target(float4(p.x, p.x, p.x, 1.0))\n",
                                             .source_name = "e.sgl",
                                             .entry_point = "main_ps"});
    REQUIRE(entry.has_error());
    CHECK(entry.error() == "e.sgl:1:11: error: unknown-name: nope\n");
}

TEST("sgl driver - a test an earlier phase found an error in is never run")
{
    // CHK-230: it would otherwise pass, beside the error that says its text is not what it seems
    auto const tested = sgl::test_source("test 1 == 1:\n    true\n", "g.sgl");
    CHECK(tested.errors == "g.sgl:1:12: error: too-many-arguments: a keyword that holds more expressions than it takes\n");
    CHECK(tested.test_count == 1);
    CHECK(tested.tests_run == 0);
}

TEST("sgl driver - every test is run or fails, and none is ever left out")
{
    // CHK-224: a function whose signature failed never has its body checked, and its test is still found and run
    auto const in_failed = sgl::test_source("fun f(x: nope) -> float:\n    test 1 == 2\n    return 1.0\n", "z.sgl");
    CHECK(in_failed.test_count == 1);
    CHECK(in_failed.tests_run == 1);
    CHECK(in_failed.errors.contains("test-failed"));

    // a test inside a test is an error of the AST pass, and is counted as one that did not check
    auto const nested = sgl::test_source("test:\n    test 1 == 1\n    true\n", "n.sgl");
    CHECK(nested.test_count == 2);
    CHECK(nested.errors.contains("declaration-not-allowed-here"));
}

TEST("sgl driver - is_same_path reads a path lexically, and a uri that does not decode matches only itself")
{
    CHECK(sgl::is_same_path("shaders/a.sgl", "./shaders//a.sgl"));
    CHECK(sgl::is_same_path("shaders/a.sgl", "shaders/sub/../a.sgl"));
    CHECK(sgl::is_same_path("shaders/a.sgl", R"(shaders\a.sgl)"));
    CHECK(sgl::is_same_path("/src/a.sgl", "file:///src/a.sgl"));
    CHECK(!sgl::is_same_path("shaders/a.sgl", "/abs/shaders/a.sgl"));
    CHECK(!sgl::is_same_path("file:///a%zz.sgl", "file:///b%zz.sgl"));
    CHECK(!sgl::is_same_path("file:///a%zz.sgl", ""));
}

TEST("sgl driver - a pipeline names no entry point that expects a diagnostic, since that one is never built")
{
    // CHK-403
    constexpr auto source = "require binding_arrays\n"
                            "binding textures:\n    maps: texture_2d[float4][16]\n    s: sampler\n"
                            "@vertex struct vertex_in:\n    position: float3\n"
                            "struct varyings:\n"
                            "    @position position: hpos4\n    uv: float2\n    @interpolate(.flat) picked: int\n"
                            "@pixel struct target:\n    color: float4\n"
                            "@vertex fun main_vs(v: vertex_in) -> varyings:\n"
                            "    return {position = hpos4(..v.position, 1.0), uv = float2(0.0, 0.0), picked = 0}\n"
                            "@expect(error = \"non-uniform-index\")\n"
                            "@pixel fun bad_ps(v: varyings){textures} -> target:\n"
                            "    return {color = textures.maps[v.picked].sample(v.uv, textures.s)}\n";
    CHECK(sgl::test_source(source, "e.sgl").errors == "");
    auto const named = sgl::test_source(cc::string(source)
                                            + "pipeline p:\n    vertex = main_vs\n    pixel = bad_ps\n"
                                              "    format = .rgba8_unorm\n",
                                        "e.sgl");
    CHECK(named.errors == "e.sgl:20:13: error: invalid-pipeline: bad_ps expects a diagnostic, so it is never built\n");
}

TEST("sgl driver - a matrix column or row outside the matrix has no value, which fails the run")
{
    // CHK-407: no target defines one, so the abstract machine stops there
    auto const tested = sgl::test_source("fun at(i: int) -> float => mat2(float2(1.0, 2.0), float2(3.0, "
                                         "4.0)).column(i).x\ntest at(2) == 0.0\n",
                                         "m.sgl");
    CHECK(tested.errors.contains("test-failed"));
    CHECK(tested.errors.contains("a matrix index outside the matrix, in a call of 'matrix_column'"));
}
