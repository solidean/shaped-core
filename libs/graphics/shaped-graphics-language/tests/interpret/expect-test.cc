#include "../check/check-test-support.hh"

#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl_test;

namespace
{
/// Every diagnostic of `source`, AST and check pass alike, with what `@expect` contains taken out, then every test that
/// does not pass as its `test-failed`: what a driver shows for the source.
cc::string outcome_of(cc::string_view source)
{
    auto const checked = check_sources(read_prelude(), source);
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});

    auto shown = checked;
    shown.module.diagnostics.clear();
    for (auto const& d : checked.user_ast.diagnostics)
        shown.module.diagnostics.push_back({.what = d, .file = checked.user_file()});
    shown.module.diagnostics.push_back_range(checked.module.diagnostics);
    sgl::test::contain_expected(checked.module, shown.module.diagnostics);
    for (auto const& r : sgl::test::run_tests(checked.module, files, {.file = checked.user_file()}))
        if (!r.is_passed())
            shown.module.diagnostics.push_back(sgl::test::diagnostic_of(checked.module, r));
    return reports_of(shown);
}
} // namespace

TEST("sgl expect - a test that expects a diagnostic contains it, and is not run")
{
    // CHK-232
    CHECK(outcome_of("@expect(error = \"type-mismatch\") test:\n    let x: int = 1.5\n    true\n") == "");
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
    CHECK(outcome_of("fun f() -> int => 1.5\n@expect(error = \"type-mismatch\") test 1 < 2\n")
          == "type-mismatch user:[1.5] expected int, got float\n"
             "unmet-expectation user:[\"type-mismatch\"] no error of kind type-mismatch stands in this test\n");
    // a warning is no error, and the other way round
    CHECK(outcome_of("@expect(error = \"no-effect\") test:\n    1 + 2\n    true\n")
          == "no-effect user:[1 + 2]\n"
             "unmet-expectation user:[\"no-effect\"] no error of kind no-effect stands in this test\n");
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
          == "test-failed user:[test] 1 of 3 checks failed\n"
             "  note user:[half(1.0) > 1.0] `half(1.0) > 1.0` is 0.5 > 1\n");
}
