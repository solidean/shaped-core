#include "../check/check-test-support.hh"

#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl_test;

namespace
{
/// Every test of `source` run, each result that is no pass as its diagnostic, in the form `reports_of` writes.
cc::string failures_of(cc::string_view source)
{
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});

    auto shown = checked;
    shown.module.diagnostics.clear();
    for (auto const& r : sgl::test::run_tests(checked.module, files, {.file = checked.user_file()}))
        if (!r.is_passed())
            shown.module.diagnostics.push_back(sgl::test::diagnostic_of(checked.module, r));
    return reports_of(shown);
}
} // namespace

TEST("sgl tests - a passing test reports nothing")
{
    CHECK(failures_of("fun square(x: float) -> float => x * x\ntest square 3.0 == 9.0\n") == "");
}

TEST("sgl tests - a failing check narrows through and, or and comparisons to the parts that were false")
{
    // the design's running example: the first conjunct holds, the second does not, and the third never ran
    CHECK(failures_of("test:\n"
                      "    let c = vec3(0.2, 1.4, 0.5)\n"
                      "    let s = vec3(saturate c.x, saturate c.y, saturate c.z)\n"
                      "    s.y <= 1.0 and (s.z > 0.6 or s.z < 0.1) and s.x == c.x\n")
          == "test-failed user:[test] 1 of 1 checks failed\n"
             "  note user:[s.z > 0.6] `s.z > 0.6` is 0.5 > 0.6\n"
             "  note user:[s.z < 0.1] `s.z < 0.1` is 0.5 < 0.1\n");
    // a part that is no comparison is false as a whole
    CHECK(failures_of("fun is_small(n: int) -> bool => n < 2\ntest is_small 3\n")
          == "test-failed user:[test] 1 of 1 checks failed\n"
             "  note user:[is_small 3] `is_small 3` is false\n");
}

TEST("sgl tests - a report names the loop variables and the test's comment")
{
    CHECK(failures_of("// counts up to two\ntest:\n    for i in 0 ..< 3:\n        i < 2\n")
          == "test-failed user:[test] 1 of 3 checks failed (counts up to two)\n"
             "  note user:[i < 2] `i < 2` is 2 < 2, with i = 2\n");
}

TEST("sgl tests - an assert stops the test, and a run that checked nothing fails")
{
    CHECK(failures_of("fun half(x: float) -> float:\n"
                      "    assert x >= 0.0\n"
                      "    return x * 0.5\n"
                      "test:\n"
                      "    half(-1.0) < 0.0\n"
                      "    true\n")
          == "test-failed user:[test] an assert failed, and the run stopped there\n"
             "  note user:[x >= 0.0] `x >= 0.0` is -1.0 >= 0.0\n");
    CHECK(failures_of("test:\n    let b = 1 > 2\n    if b:\n        true\n")
          == "test-failed user:[test] the run ran no check\n");
}

TEST("sgl tests - a report spells a whole float as one, narrows through not, and counts asserts apart")
{
    // `2.0`, never `2`: a value reads as the type it is
    CHECK(failures_of("test 1.5 + 0.25 > 2.0\n")
          == "test-failed user:[test] 1 of 1 checks failed\n"
             "  note user:[1.5 + 0.25 > 2.0] `1.5 + 0.25 > 2.0` is 1.75 > 2.0\n");
    // EVAL-79: a `not` of a comparison that held shows that comparison's values
    CHECK(failures_of("fun sq(x: int) -> int => x * x\ntest not (sq(2) == 4)\n")
          == "test-failed user:[test] 1 of 1 checks failed\n"
             "  note user:[not (sq(2) == 4)] `not (sq(2) == 4)` is not (4 == 4)\n");
    // the asserts of a callee are no checks of the test, and the report says so beside them
    CHECK(failures_of("fun half(x: float) -> float:\n    assert x >= 0.0\n    return x * 0.5\n"
                      "test:\n    for i in 0 ..< 2:\n        half(i as float) > 0.25\n")
          == "test-failed user:[test] 1 of 2 checks failed, 2 asserts held\n"
             "  note user:[half(i as float) > 0.25] `half(i as float) > 0.25` is 0.0 > 0.25, with i = 0\n");
}

namespace
{
/// Every site of every test of `source`, one per line: `<text> <passed>/<failed>`, and `assert` before an assert's.
cc::string marks_of(cc::string_view source)
{
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});

    auto out = cc::string();
    for (auto t = isize(0); t < checked.module.tests.size(); ++t)
        for (auto const& s : sgl::test::run_test(checked.module, files, i32(t)).sites)
            out.appendf("{}{} {}/{}\n", s.is_assert ? "assert " : "", checked.files[s.file].text_of(s.where), s.passed,
                        s.failed);
    return out;
}
} // namespace

TEST("sgl tests - every site of a run counts how often it held and failed, and one never reached counts neither")
{
    CHECK(marks_of("test:\n    1 < 2\n    2 < 1\n") == "1 < 2 1/0\n2 < 1 0/1\n");
    // a check in a loop can do both
    CHECK(marks_of("test:\n    for i in 0 ..< 3:\n        i < 2\n") == "i < 2 2/1\n");
    // a check the run never reaches is a site all the same
    CHECK(marks_of("test:\n    let b = 1 > 2\n    if b:\n        false\n    true\n") == "false 0/0\ntrue 1/0\n");
    // an assert in a helper is a site of the test that reaches it, in the helper's own place
    CHECK(marks_of("fun half(x: float) -> float:\n    assert x >= 0.0\n    return x * 0.5\ntest half(2.0) == 1.0\n")
          == "half(2.0) == 1.0 1/0\nassert assert x >= 0.0 1/0\n");
}

TEST("sgl tests - one test runs alone, and a test that expects diagnostics is judged by them rather than run")
{
    auto const checked
        = check_sources(read_prelude(), "test 1 < 2\n@expect(error = \"unknown-name\")\ntest nope\ntest nope\n");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});
    REQUIRE(checked.module.tests.size() == 3);
    CHECK(sgl::test::run_test(checked.module, files, 0).is_passed());
    CHECK(sgl::test::run_test(checked.module, files, 1).status == sgl::test::test_status::judged_by_diagnostics);
    // the same body without the expectation did not check, so there is nothing to run
    CHECK(sgl::test::run_test(checked.module, files, 2).status == sgl::test::test_status::not_run);
}

TEST("sgl tests - a raised stop flag ends a run as stopped, and nothing is judged against it")
{
    auto const checked = check_sources(read_prelude(), "@expect(.fail)\ntest:\n    let mut i = 0\n    while i >= 0:\n  "
                                                       "      i = i + 1\n    i < 0\n");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});
    auto stop = cc::atomic<bool>(true);
    auto const r = sgl::test::run_test(checked.module, files, 0, {.stop = &stop});
    // `stopped` rather than `passed`: the `@expect(.fail)` was not judged against the unfinished run
    CHECK(r.status == sgl::test::test_status::stopped);
    CHECK(!r.is_passed()); // a stopped run is no pass
}

TEST("sgl tests - an integer divisor of zero is a program error, and no expectation of a failed check passes on it")
{
    // Division by zero has no value on any target, so the interpreter stops rather than invent one.
    // `@expect(.fail)` expects a false check, which this is not, so the test with it fails too.
    auto const checked = check_sources(read_prelude(), "test:\n    let zero = 0\n    7 / zero == 0\n"
                                                       "@expect(.fail)\ntest:\n    let zero = 0\n    7 % zero == 1\n"
                                                       "test:\n    let n = -2147483647 - 1\n    n / -1 == n\n"
                                                       "test:\n    let zero: uint = 0\n    (7 as uint) / zero == 0\n");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});
    REQUIRE(checked.module.tests.size() == 4);
    for (auto i = isize(0); i < 4; ++i)
    {
        auto const r = sgl::test::run_test(checked.module, files, i);
        CHECK(r.status == sgl::test::test_status::program_error).dump("test", i);
        CHECK(!r.is_passed()).dump("test", i);
    }
    CHECK(sgl::test::run_test(checked.module, files, 0).detail.contains("divided by zero"));
    CHECK(sgl::test::run_test(checked.module, files, 2).detail.contains("most negative int"));
}

TEST("sgl tests - a maths builtin outside the domain WGSL defines it on is a program error")
{
    auto const checked = check_sources(read_prelude(), "test:\n    let x = -2.0\n    pow(x, 0.5) > 0.0\n"
                                                       "test:\n    let zero = 0.0\n    pow(zero, zero) == 1.0\n"
                                                       "test:\n    let x = 2.0\n    asin(x) > 0.0\n"
                                                       "test:\n    let e = 1.0\n    smoothstep(e, e, 0.5) == 0.0\n"
                                                       "test:\n    let x = 2.0\n    pow(x, 0.5) > 1.0\n");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = checked.files[i], .ast = checked.asts[i]});
    REQUIRE(checked.module.tests.size() == 5);
    for (auto i = isize(0); i < 4; ++i)
        CHECK(sgl::test::run_test(checked.module, files, i).status == sgl::test::test_status::program_error).dump("test", i);
    // inside the domain it is an ordinary value
    CHECK(sgl::test::run_test(checked.module, files, 4).is_passed());
}
