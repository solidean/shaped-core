#include "../check/check-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>

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
    CHECK(sgl::test_source("test:\n    1 + 2\n    true\n", "w.sgl").warnings == "w.sgl:2:5: warning: no-effect\n");
}

TEST("sgl driver - a const that did not check fails what reads it silently, and never crashes")
{
    // CHK-19: a failed symbol is the error type where it is named, and the one diagnostic is its own
    cc::string_view const broken[] = {
        "const a = nope\n",
        "const a: float = 3\n",
        "const a = 1 + 2\n",
        "const a = 2147483648\n",
        "const a = b\nconst b = a\n",
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
    CHECK(sgl::test_source("enum m:\n    a\n    b\nconst bad = 1 + 2\nfun f(x: m) -> int:\n    return case x:\n"
                           "        bad => 1\n        _ => 0\ntest f(m.a) == 0\n",
                           "p.sgl")
              .errors.contains("unsupported-yet"));
    auto const entry = sgl::compile_to_text(
        {.source = "const a = nope\nstruct frag:\n    x: float\n@pixel struct target:\n    color: float4\n"
                   "@pixel fun main_ps(p: frag) -> target:\n    let w = a\n    return target(float4(p.x, p.x, p.x, 1.0))\n",
         .source_name = "e.sgl",
         .entry_point = "main_ps"});
    REQUIRE(entry.has_error());
    CHECK(entry.error() == "e.sgl:1:11: error: unknown-name: nope\n");
}
