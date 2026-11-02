#include "../check/check-test-support.hh"

#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl_test;

namespace
{
/// The results of every test of `checked`, which must have checked clean.
cc::vector<sgl::test::test_result> results_of(checked_sources const& checked)
{
    REQUIRE(reports_of(checked) == "");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = *checked.files[i], .ast = *checked.asts[i]});
    return sgl::test::run_tests(checked.module, files, {.file = checked.user_file()});
}

/// What the one test of `source` printed, a line each.
cc::string printed_by(cc::string_view source)
{
    auto const checked = check_sources(read_prelude(), source);
    auto const results = results_of(checked);
    REQUIRE(results.size() == 1);
    auto out = cc::string();
    for (auto const& line : results[0].printed)
        out.appendf("{}\n", line);
    return out;
}
} // namespace

TEST("sgl messages - a print writes its interpolations into its text, each as the value it is")
{
    CHECK(printed_by("test:\n"
                     "    let p = float3(1.0, 2.5, -3.0)\n"
                     "    let n = 4\n"
                     "    print \"p = $p, x = $p.x, twice = $(n * 2)!\"\n"
                     "    true\n")
          == "p = float3(1.0, 2.5, -3.0), x = 1.0, twice = 8!\n");
}

TEST("sgl messages - a print in a loop writes once per run, in order")
{
    CHECK(printed_by("test:\n"
                     "    for i in 0 ..< 3:\n"
                     "        print \"i=$i\"\n"
                     "    true\n")
          == "i=0\ni=1\ni=2\n");
}

TEST("sgl messages - escapes are resolved and `$$` is a dollar")
{
    CHECK(printed_by("test:\n"
                     "    print \"costs $$5\\tnow \\\"quoted\\\" \\u{41}\"\n"
                     "    true\n")
          == "costs $5\tnow \"quoted\" A\n");
}

TEST("sgl messages - an interpolation that calls a function runs its body where the message stands")
{
    CHECK(printed_by("fun clamp01(x: float) -> float:\n"
                     "    if x < 0.0 => return 0.0\n"
                     "    return min(x, 1.0)\n"
                     "test:\n"
                     "    print \"$(clamp01(-2.0)) and $(clamp01(0.5))\"\n"
                     "    true\n")
          == "0.0 and 0.5\n");
}

TEST("sgl messages - a failed assert reports its message with the values that failed it")
{
    auto const checked = check_sources(read_prelude(), "fun half(x: float) -> float:\n"
                                                       "    assert x >= 0.0, \"half of $x, which is negative\"\n"
                                                       "    return x * 0.5\n"
                                                       "test half(-3.0) == -1.5\n");
    auto const results = results_of(checked);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].failures.size() == 1);
    CHECK(results[0].failures[0].message == "half of -3.0, which is negative");
    auto const d = sgl::test::diagnostic_of(checked.module, results[0]);
    auto says = false;
    for (auto const& note : d.notes)
        says = says || note.message == "it says: half of -3.0, which is negative";
    CHECK(says);
}

TEST("sgl messages - an assert that holds computes its message and reports nothing")
{
    auto const checked = check_sources(read_prelude(), "test:\n"
                                                       "    let v = 2.0\n"
                                                       "    assert v > 1.0, \"v is $v\"\n"
                                                       "    true\n");
    auto const results = results_of(checked);
    REQUIRE(results.size() == 1);
    CHECK(results[0].is_passed());
}

TEST("sgl messages - a string is no value, so it stands only as a message")
{
    CHECK(reports_of(check_sources(read_prelude(), "fun f() -> float:\n    let s = \"text\"\n    return 1.0\n"))
              .contains("unsupported-yet user:[\"text\"] a string literal"));
    CHECK(reports_of(check_sources(read_prelude(), "fun f(x: float):\n    assert x > 0.0, x\n"))
              .contains("unsupported-yet user:[x] an assert message that is no quoted literal"));
}

TEST("sgl messages - an interpolation is a scalar, a plain vector or an enum")
{
    auto const reports = reports_of(check_sources(read_prelude(), "struct pair:\n"
                                                                  "    a: float\n"
                                                                  "    b: float\n"
                                                                  "fun f(p: pair):\n"
                                                                  "    print \"p is $p\"\n"));
    CHECK(reports.contains("type-mismatch user:[p] a message interpolates a scalar, a plain vector or an enum, and "
                           "this is pair"));
}
