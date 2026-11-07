#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh>
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
//                 "started": 1762500000.25, "ended": 1762500000.37,
//                 "large_peak_bytes": 4194304, "large_allocations": 2,
//                 "files": [{"path": "...", "read": true, "written": false, "listed": false,
//                            "bytes_read": 10, "bytes_written": 0}]}],
//      "unattributed": {"files": [...]}}
//
// One entry per scheduled test; a dispatched child is folded into its driver, which fails when any child fails.
// "started" and "ended" are Unix epoch seconds, so a runner sampling the process's memory from outside can tell which
// tests ran during each sample.
// "large_peak_bytes" and "large_allocations" come from cc::large_allocations (blocks of 1 MiB and more, booked to the
// test that allocated them and credited back when freed); present only for a test that allocated one.
// "version" changes with anything a reader must know about.
//
// Sampled functions, when the runner also sets NX_EXTERNAL_RUNNER_SAMPLE_HZ to a rate (per thread, in Hz):
// clean-core's sampler (cc::rec sampling, with a sink) walks every recording thread at that rate for the whole run,
// files each stack under the test whose pass owner (cc::rec::owner_scope) the thread was under, and the report says,
// per test, how many samples it got and which code addresses they hit, module-relative:
//
//     {"modules": [{"path": "C:/b/x-test.exe", "identity": "65A1B2C3d000"}, ...],
//      "sampling": {"hz": 200, "taken": 5120, "failed": 0},
//      "tests": [{..., "body": [0, 4096], "samples": 31, "sampled": [[0, 4100, 5230], [1, 812]]}],
//      "unattributed": {..., "samples": 4, "sampled": [...]}}
//
// "sampled" lists per module (its index first) the sorted relative addresses; a caller's frame is its return address
// minus one, so every address lies inside the instruction that was running or calling.
// "body" is where the test's body starts, when the build keeps code addresses for source sites.
// A sample is a statistical witness that code ran in a test, never a proof that other code did not.
//
// Covered functions, when the runner also sets NX_EXTERNAL_RUNNER_COVERAGE to 1 and the build counts coverage
// (clang's -fprofile-instr-generate, as the coverage presets build): every test then runs alone, and nexus reads and
// resets the profile counters as each top-level test begins and ends, so per test the report names exactly the
// functions it entered, as indices into the run's table of the profile's function name hashes:
//
//     {"coverage": {"functions": [1234567890123, ...]},
//      "tests": [{..., "covered": [0, 17, 18]}],
//      "unattributed": {..., "covered": [...]}}
//
// A name hash is the profile's own (the low 64 bits of the MD5 of the function's profile name), which the build's
// coverage mapping (llvm-cov) maps to a file and a line; "unattributed" is what ran outside every test.
// A child process is not counted, and work a test left running counts for whichever test runs while it does.

/// What the sampler saw under one context: how many samples, and the distinct code addresses they hit.
struct nx::sampled_context
{
    isize samples = 0;
    /// Absolute, sorted; a caller's frame is its return address minus one.
    cc::vector<u64> addresses;
};

/// What the report's sampler saw over a run, by context (a scheduled test's declaration, 0 for none).
struct nx::sampled_functions
{
    f64 hz = 0;
    u64 taken = 0;
    u64 failed = 0;
    cc::map<u64, sampled_context> by_context;
    /// The modules mapped when the run ended, which the addresses are placed in.
    cc::vector<cc::loaded_module> modules;
};

/// What the report's coverage counted over a run: the profile's functions, and per context the ones entered.
struct nx::covered_functions
{
    /// The profile's function name hashes, in the runtime's record order.
    cc::vector<u64> functions;
    /// By context (a scheduled test's declaration, 0 for none): sorted indices into `functions`.
    cc::map<u64, cc::vector<i32>> by_context;
};

namespace nx
{
/// The variable naming the report's path; unset, nothing is recorded.
inline constexpr char const* runner_report_variable = "NX_EXTERNAL_RUNNER_REPORT";

/// The variable asking for sampled functions in the report: a rate in Hz; unset or 0, nothing is sampled.
inline constexpr char const* runner_report_sampling_variable = "NX_EXTERNAL_RUNNER_SAMPLE_HZ";

/// The variable asking for covered functions in the report: 1 asks, and the build must count coverage.
inline constexpr char const* runner_report_coverage_variable = "NX_EXTERNAL_RUNNER_COVERAGE";

/// Whether this build counts coverage, so the report can name each test's covered functions.
[[nodiscard]] bool build_counts_coverage();

/// Turns file-access recording on and attributes each access to the running test; call once, before any test runs.
/// A positive `sample_hz` also samples every recording thread at that rate, for the report's sampled functions.
/// `coverage` also counts each test's covered functions, where the build counts coverage;
/// the run must then run every test alone (test_schedule_config::one_at_a_time).
void begin_runner_report(f64 sample_hz = 0, bool coverage = false);

/// Stops recording and returns the report of `execution`.
cc::string end_runner_report(test_schedule_execution const& execution, cc::optional<u64> run_seed);

/// The report of `execution` over `accesses` and `large`; contexts are the scheduled tests' declarations, 0 for none.
/// With `sampled`, each test also carries its sampled addresses and where its body starts; with `covered`, its covered functions.
cc::string write_runner_report(test_schedule_execution const& execution,
                               cc::span<cc::file_access::record const> accesses,
                               cc::span<cc::large_allocations::context_stats const> large,
                               cc::optional<u64> run_seed,
                               sampled_functions const* sampled = nullptr,
                               covered_functions const* covered = nullptr);
} // namespace nx

namespace nx::impl
{
/// Notes that samples under `owner` belong to the scheduled test `slot`; a no-op unless the report samples.
/// Also starts the report's sampler again where a test that owned the recorder stopped it.
void note_pass_owner(u64 owner, test_declaration const* slot);

/// Stops the report's sampler before the recorder goes down; the next pass under an owner starts it again.
void pause_report_sampling();

/// What the report's sampler has seen so far, or nothing when the report does not sample; stops nothing.
[[nodiscard]] cc::optional<sampled_functions> report_samples();

/// Stops the report's sampler for good and returns what it saw, forgetting it.
cc::optional<sampled_functions> end_report_sampling();

/// Starts counting the report's coverage; what ran before, static setup above all, is no test's.
void begin_report_coverage();

/// The coverage boundaries of a top-level test: what ran since the last boundary goes to the test running then
/// (none between tests), and the counters start over.
/// A no-op unless the report covers; a dispatched child or a nested run's test counts for the test it ran in.
void note_coverage_begin(test_declaration const* slot);
void note_coverage_end();

/// Stops counting and returns what the report's coverage counted, or nothing when it does not cover.
cc::optional<covered_functions> end_report_coverage();
} // namespace nx::impl
