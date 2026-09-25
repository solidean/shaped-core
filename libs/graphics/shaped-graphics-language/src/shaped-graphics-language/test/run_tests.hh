#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/interpret/interpret.hh>

/// Runs the `test`s of a checked module on the interpreter, and says why a failing one failed.
///
/// A failing check is narrowed: through `and`, `or`, `not` and comparison chains down to the parts that were false,
/// each with the values that made it so, which is what a reader needs to see without rerunning anything.

enum class sgl::test::test_status : sgl::u8
{
    passed,
    /// A check was false; the run went on, and every failure is in `test_result::failures`.
    failed,
    /// An `assert` was false, in the test or in anything it calls, and the run stopped there.
    assertion_failed,
    out_of_fuel,
    /// The run ended without running a single check, which a test that checks by its asserts ends in `true` to avoid.
    no_check_ran,
    /// The test has no flat tree: its body did not check, which a diagnostic already says.
    not_run,
    /// The interpreter met a tree the check pass should not have written; a bug of the compiler, and not of the test.
    internal_error,
};

/// One part of a failing condition that was false, and the values that made it so.
struct sgl::test::narrowed_part
{
    i32 file = 0;
    source_span where;
    /// The part as the source writes it: `s.z > 0.6`.
    cc::string text;
    /// What it was: `0.5 > 0.6` for a comparison, `false` for a part that is no comparison.
    cc::string values;
};

/// One check or `assert` that was false.
struct sgl::test::check_report
{
    i32 file = 0;
    /// The whole check, as the source writes it.
    source_span where;
    cc::string text;
    bool is_assert = false;
    /// In the order the condition writes them; empty for a condition that was false as a whole.
    cc::vector<narrowed_part> parts;
    /// `i = 3`, one per `for` of the test around the check, outermost first.
    cc::vector<cc::string> loop_values;
};

struct sgl::test::test_result
{
    /// A position in `checked_module::tests`.
    i32 test = -1;
    test_status status = test_status::not_run;
    cc::vector<check_report> failures;
    /// The failures `run_limits::max_failures` left out.
    i32 failures_dropped = 0;
    i32 checks_run = 0;
    /// What the interpreter said about a run that stopped for another reason than a check.
    cc::string detail;

    [[nodiscard]] bool is_passed() const { return status == test_status::passed; }
};

struct sgl::test::test_options
{
    /// Only the tests of this file, a position in the files `check` was given; -1 runs every test of the module.
    i32 file = -1;
    check::run_limits limits;
};

namespace sgl::test
{

/// `passed`, `failed`, `assertion-failed`, `out-of-fuel`, `no-check-ran`, `not-run`, `internal-error`.
[[nodiscard]] cc::string_view to_string(test_status s);

/// Every test of `m` the options select, in the order `m.tests` holds them.
/// `files` are the files `m` was checked from, in the same order, since a report quotes the source.
[[nodiscard]] cc::vector<test_result> run_tests(check::checked_module const& m,
                                                cc::span<check::module_file const> files,
                                                test_options const& options = {});

/// A result that is no pass as the diagnostic `test-failed` at its test, with one related note per narrowed part.
[[nodiscard]] check::located_diagnostic diagnostic_of(check::checked_module const& m, test_result const& r);

/// A value as a report writes it: `0.5`, `3`, `true`, `vec3(1, 2, 3)`, `light_kind.sun`, `void`.
[[nodiscard]] cc::string text_of_value(check::checked_module const& m, check::value const& v);
} // namespace sgl::test
