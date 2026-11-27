#include "../check/check-test-support.hh"

#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl_test;

namespace
{
/// Every diagnostic of `source`, AST and check pass alike, with what `@expect` contains taken out, then every test that
/// does not pass as its `test-failed`: what a driver shows for the source.
cc::string outcome_of(cc::string_view source)
{
    auto checked = check_sources(read_prelude(), source);
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = *checked.files[i], .ast = *checked.asts[i]});

    auto shown = cc::vector<sgl::check::located_diagnostic>();
    for (auto const& d : checked.user_ast.diagnostics)
        shown.push_back({.what = d, .file = checked.user_file()});
    shown.push_back_range(checked.module.diagnostics);
    sgl::test::contain_expected(checked.module, shown);
    for (auto const& r : sgl::test::run_tests(checked.module, files, {.file = checked.user_file()}))
        if (!r.is_passed())
            shown.push_back(sgl::test::diagnostic_of(checked.module, r));
    // the module is done with, so its list is what `reports_of` shows
    checked.module.diagnostics = cc::move(shown);
    return reports_of(checked);
}
} // namespace

TEST("sgl expect - a test that expects a diagnostic contains it, and is not run")
{
    // CHK-232
    CHECK(outcome_of("@expect(error = \"type-mismatch\") test:\n    let x: int = true\n    true\n") == "");
    CHECK(outcome_of("@expect(warning = \"no-effect\") test:\n    1 + 2\n    true\n") == "");
    // a glob, on a test in a function body
    CHECK(outcome_of("fun f(k: float) -> float:\n"
                     "    @expect(error = \"test-captures-*\") test k > 0.0\n"
                     "    return k\n")
          == "");
}

TEST("sgl expect - an expectation nothing meets is an error, and a diagnostic outside the test is never taken")
{
    CHECK(outcome_of("@expect(error = \"unknown-name\") test 1 < 2\n")
          == "unmet-expectation user:[\"unknown-name\"] no error of kind unknown-name stands in this test\n");
    CHECK(outcome_of("fun f() -> int => true\n@expect(error = \"type-mismatch\") test 1 < 2\n")
          == "type-mismatch user:[true] expected int, got bool\n"
             "unmet-expectation user:[\"type-mismatch\"] no error of kind type-mismatch stands in this test\n");
    // a warning is no error, and the other way round
    // An unmet expectation hands back what the test took, since that is the likeliest reason it went unmet.
    CHECK(outcome_of("@expect(error = \"no-effect\") test:\n    1 + 2\n    true\n")
          == "unmet-expectation user:[\"no-effect\"] no error of kind no-effect stands in this test\n"
             "no-effect user:[1 + 2]\n");
    // CHK-232: one expected error brings others with it, and the test takes them all
    CHECK(outcome_of("@expect(error = \"unknown-name\") test:\n    let x = missing\n    x + 1\n    true\n") == "");
    // CHK-231: an empty kind can meet nothing, so it is refused where it is written
    CHECK(outcome_of("@expect(error = \"\") test 1 < 2\n").starts_with("invalid-attribute-arguments"));
    // `?` stands for one character, as `*` stands for a run of them
    CHECK(outcome_of("@expect(error = \"unknown-nam?\") test missing < 1\n") == "");
    CHECK(outcome_of("@expect(maybe) test 1 < 2\n")
          == "invalid-attribute-arguments user:[maybe] @expect names what the test does: .fail, .assert, error = "
             "\"kind\" "
             "or warning = \"kind\"\n");
}

TEST("sgl expect - a test that is to fail passes by failing")
{
    CHECK(outcome_of("@expect(.fail) test 1 > 2\n") == "");
    CHECK(outcome_of("@expect(.fail) test 1 < 2\n") == "test-failed user:[test] it was to fail, and it passed\n");

    constexpr auto half = cc::string_view("fun half(x: float) -> float:\n"
                                          "    assert x >= 0.0\n"
                                          "    return x * 0.5\n");
    CHECK(outcome_of(cc::string(half) + "@expect(.assert) test:\n    half(-1.0) < 0.0\n    true\n") == "");
    // a false check is no assert, and the assert that held counts among the checks that ran
    CHECK(outcome_of(cc::string(half) + "@expect(.assert) test:\n    half(1.0) > 1.0\n    true\n")
          == "test-failed user:[test] 1 of 2 checks failed, 1 asserts held\n"
             "  note user:[half(1.0) > 1.0] `half(1.0) > 1.0` is 0.5 > 1.0\n");
}

namespace
{
/// A pixel stage under forced early depth that discards, which warns `early-depth-keeps-discarded` (CHK-566).
cc::string cutout(cc::string_view attributes)
{
    return cc::format("struct varyings:\n"
                      "    @position position: hpos4\n"
                      "    uv: float2\n"
                      "{}"
                      "@early_depth_test\n"
                      "@pixel fun cut_ps(p: varyings) -> float4:\n"
                      "    require early_depth_test\n"
                      "    if p.uv.x > 0.5 => discard\n"
                      "    return float4(1.0)\n",
                      attributes);
}
} // namespace

TEST("sgl allow - an entry point that allows a warning of its body reports it nowhere, and is built")
{
    // CHK-566: the warning the allowance is for
    CHECK(outcome_of(cutout("")).starts_with("early-depth-keeps-discarded user:[discard]"));
    // CHK-576
    CHECK(outcome_of(cutout("@allow(\"early-depth-keeps-discarded\")\n")) == "");
    // an allowance written twice allows as one
    CHECK(outcome_of(cutout("@allow(\"early-depth-keeps-discarded\")\n@allow(\"early-depth-keeps-discarded\")\n"))
          == "");
    auto const checked = check_sources(read_prelude(), cutout("@allow(\"early-depth-keeps-discarded\")\n"));
    CHECK(checked.module.entry_points.size() == 1);
}

TEST("sgl allow - an allowance nothing meets is unmet, a diagnostic of its entry point alone")
{
    // CHK-577: the stage keeps every pixel it runs, so nothing warns
    constexpr auto whole = cc::string_view("struct varyings:\n"
                                           "    @position position: hpos4\n"
                                           "@allow(\"early-depth-keeps-discarded\")\n"
                                           "@early_depth_test\n"
                                           "@pixel fun whole_ps(p: varyings) -> float4 => float4(1.0)\n");
    CHECK(outcome_of(whole)
          == "unmet-expectation user:[\"early-depth-keeps-discarded\"] no warning of kind early-depth-keeps-discarded is "
             "judged of this entry point, so it allows nothing\n");
    auto checked = check_sources(read_prelude(), whole);
    auto shown = checked.module.diagnostics;
    sgl::test::contain_expected(checked.module, shown);
    REQUIRE(shown.size() == 1);
    REQUIRE(shown[0].entry_points.size() == 1);
    CHECK(checked.module.at(shown[0].entry_points[0]).name == "whole_ps");
    // a warning of another kind is none it meets, and stays reported
    auto const other = outcome_of(cutout("@allow(\"nan-test-at-fast\")\n"));
    CHECK(other.contains("early-depth-keeps-discarded user:[discard]"));
    CHECK(other.contains("unmet-expectation user:[\"nan-test-at-fast\"]"));
}

TEST("sgl allow - an allowed warning is nothing an @expect meets, and one that expects is never judged for it")
{
    // CHK-576: taken before CHK-403's expectations
    CHECK(outcome_of(cutout("@allow(\"early-depth-keeps-discarded\")\n@expect(warning = "
                            "\"early-depth-keeps-discarded\")\n"))
              .starts_with("unmet-expectation user:[\"early-depth-keeps-discarded\"] no warning of kind "
                           "early-depth-keeps-discarded is judged of this entry point\n"));
    // CHK-577: an entry point that is never built raised nothing an allowance could meet
    constexpr auto expecting = cc::string_view("struct varyings:\n"
                                               "    @position position: hpos4\n"
                                               "@allow(\"early-depth-keeps-discarded\")\n"
                                               "@expect(error = \"feature-not-declared\")\n"
                                               "@early_depth_test\n"
                                               "@pixel fun bad_ps(p: varyings) -> float4:\n"
                                               "    if p.position.x < 0.5 => discard\n"
                                               "    return float4(1.0)\n");
    CHECK(outcome_of(expecting) == "");
}

TEST("sgl allow - an allowance names warning kinds, exactly, on an entry point")
{
    // CHK-576
    auto const refused = [](cc::string_view attribute)
    { return outcome_of(cutout(cc::format("{}\n", attribute))).starts_with("invalid-attribute-arguments"); };
    CHECK(refused("@allow()"));
    CHECK(refused("@allow(warning = \"early-depth-keeps-discarded\")"));
    CHECK(refused("@allow(early)"));
    CHECK(refused("@allow(\"\")"));
    CHECK(refused("@allow(\"early-depth-*\")"));
    CHECK(refused("@allow(\"no-such-warning\")"));
    // an error is never intended
    CHECK(outcome_of(cutout("@allow(\"type-mismatch\")\n"))
              .contains("invalid-attribute-arguments user:[\"type-mismatch\"] 'type-mismatch' is an error, which no "
                        "entry point allows"));
    // a helper has no inlined body of its own to judge
    CHECK(outcome_of("@allow(\"no-effect\")\nfun helper() -> float => 1.0\n")
          == "invalid-attribute-arguments user:[allow] only an entry point allows a warning, of its inlined body\n");
}

TEST("sgl allow - every diagnostic kind is found by the name it is spelled as")
{
    for (auto k = 0; k <= int(sgl::k_last_diagnostic_kind); ++k)
    {
        auto const kind = sgl::diagnostic_kind(k);
        auto const named = sgl::diagnostic_kind_named(sgl::to_string(kind));
        REQUIRE(named.has_value());
        CHECK(named.value() == kind);
    }
    CHECK(!sgl::diagnostic_kind_named("no-such-kind").has_value());
}
