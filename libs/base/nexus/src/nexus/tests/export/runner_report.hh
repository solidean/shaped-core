#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

// The report an external test runner asks for: per test, how it ended and every file it read, wrote or listed.
//
// A runner that caches test results, or schedules them by what they touch, sets NX_EXTERNAL_RUNNER_REPORT to a path.
// nexus then records file access through clean-core (cc::file_access) for the whole run, attributes each access to
// the test it happened in (on any thread, through the ambient chain), and writes this JSON beside its other reports.
// Accesses outside any test (static setup, a thread no test started) are listed under "unattributed": they count for every test.
// Access past clean-core is not seen unless declared (cc::file_access::note_read and siblings).
//
//     {"format": "nexus-runner-report", "version": 1, "seed": 123,
//      "tests": [{"name": "...", "outcome": "pass" | "fail" | "skip", "seconds": 0.12,
//                 "files": [{"path": "...", "read": true, "written": false, "listed": false,
//                            "bytes_read": 10, "bytes_written": 0}]}],
//      "unattributed": {"files": [...]}}
//
// One entry per scheduled test; a dispatched child is folded into its driver, which fails when any child fails.
// "version" changes with anything a reader must know about.

namespace nx
{
/// The variable naming the report's path; unset, nothing is recorded.
inline constexpr char const* runner_report_variable = "NX_EXTERNAL_RUNNER_REPORT";

/// Turns file-access recording on and attributes each access to the running test; call once, before any test runs.
void begin_runner_report();

/// Stops recording and returns the report of `execution`.
cc::string end_runner_report(test_schedule_execution const& execution, cc::optional<u64> run_seed);

/// The report of `execution` over `accesses`; contexts are the scheduled tests' declarations, 0 for none.
cc::string write_runner_report(test_schedule_execution const& execution,
                               cc::span<cc::file_access::record const> accesses,
                               cc::optional<u64> run_seed);
} // namespace nx
