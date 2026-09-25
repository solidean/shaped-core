#include "../check/check-test-support.hh"

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
