#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/result.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/schedule.hh>

namespace nx
{
struct recorded_metric;
struct test_error;
struct test_execution;
struct test_schedule_execution;
struct test_serial_time;
struct test_run_resources;
} // namespace nx

// Forward declaration for impl namespace
namespace nx::impl
{
enum class check_kind;
enum class cmp_op;
} // namespace nx::impl

struct nx::test_error
{
    cc::string expr; // usually not shown in editor (assumed to be part of source already)
    cc::source_location location;
    cc::vector<cc::string> extra_lines;
    cc::string expanded; // shown inline next to the code at location (important for VSCode DX)
    // NOTE: if expr == expanded, C++ TestMate just shows "failed" instead of anything useful, so make sure they are always different
};

// A single performance metric recorded by a PGO benchmark via nx::pgo (see pgo.hh).
//
// The unit is a `cc::rec::unit` rather than a label, so it carries the symbol, the prefix base AND the orientation
// that decides whether a delta is an improvement.
// It points at a static object the recording site owns, which is what makes storing a pointer safe here.
struct nx::recorded_metric
{
    cc::string name;
    double value = 0;
    cc::rec::unit const* unit = nullptr;

    [[nodiscard]] bool higher_is_better() const;
    [[nodiscard]] cc::string_view unit_symbol() const;
};

struct nx::test_execution
{
    test_instance instance;

    // Metrics recorded by nx::pgo during this test (typically a PGO benchmark). Empty for normal tests.
    cc::vector<recorded_metric> metrics;

    // Loops measured by nx::bench::run during this test, in declaration order.
    // Empty for everything but a BENCHMARK.
    //
    // Order is what the comparison table keys its baseline on, so this is a vector rather than a map: the first loop
    // declared is the baseline unless one asked to be it.
    cc::vector<bench::result> benchmarks;

    struct section
    {
        cc::string name;
        cc::source_location location;
        cc::vector<section> subsections;

        // NOTE: only valid for leaf sections
        cc::vector<test_error> errors;

        // stats
        int executed_checks = 0;
        int failed_checks = 0;
        double duration_seconds = 0.0;

        // result
        bool is_considered_failing = false;
    };

    // note: global stats == root stats
    section root;

    // The interval the test occupied, on cc::current_time_steady_secs(), and 0 for a test that never started.
    // Unlike root.duration_seconds, which sums the section passes, this is one interval, so it places the test on a timeline.
    double started_at_steady_s = 0.0;
    double finished_at_steady_s = 0.0;

    // The thread the body started on — the counter cc::current_thread_id() hands out, not an OS id.
    u64 thread = 0;

    // The --verbose console trace this test produced, buffered instead of printed as it happens.
    // Tests may run concurrently, so printing from the running test's own thread interleaves into noise.
    // A nested (dispatched) execution appends into its top-level ancestor's buffer, which is what keeps a driver's trace and its children's interleaved as they were.
    cc::string verbose_output;

    // Executions dispatched from this test's body via nx::invoke_tests, so a parametrized-test instance runs as an addressable child.
    // Empty for an ordinary test.
    // invocation_group is the nx::invoke_tests(name) segment this execution ran under, and is empty for a top-level test.
    // The child's own name is instance.declaration->name, so its addressable path is invocation_group / declaration name / sections.
    cc::vector<test_execution> nested;
    cc::string invocation_group;

    // What a COMMAND's body returned; empty for anything that is not a command, or a command that never returned.
    cc::optional<int> exit_code;

    // Failing if this test's own tree fails or any dispatched child fails.
    [[nodiscard]] bool is_considered_failing() const;
};

struct nx::test_schedule_execution
{
    cc::vector<test_execution> executions;

    // Checks that ran with no test to attribute them to — see report_check_result: a thread nexus never heard of, or code outside a test entirely.
    // They belong to no test, so they fail the RUN rather than a test, however green everything else is.
    // Drained from a process-global sink at the end of this run, so a nested execute_tests takes what it produced and the outer one sees only its own.
    int orphan_checks = 0;
    cc::vector<test_error> orphan_errors;

    // All counts recurse into dispatched (nested) executions: a dispatched instance counts as its own test.
    [[nodiscard]] int count_total_tests() const;
    [[nodiscard]] int count_failed_tests() const;
    [[nodiscard]] int count_total_checks() const;
    [[nodiscard]] int count_failed_checks() const;

    /// The floor of this run that no scheduling could have overlapped, read off where each top-level test sat and what it held.
    [[nodiscard]] test_serial_time serial_time() const;
};

/// How much of a run was serial by declaration rather than by accident — what no --jobs could have shortened.
struct nx::test_serial_time
{
    /// Tests that overlapped nothing: exclusive() in the shared phase, plus the whole span of every other phase.
    /// Phases run one after another, so a no_scheduler, singlethreaded or own_pool test is alone relative to the shared phase.
    double alone_s = 0;

    /// The exclusion group whose holders took longest in sum: a tag, or "main_thread", whose bodies share the one run thread.
    /// Groups exclude only their own holders, so the groups run beside each other and only the largest adds to the floor.
    cc::string largest_group;
    double largest_group_s = 0;

    [[nodiscard]] double total_s() const { return alone_s + largest_group_s; }
};

/// What a run cost the machine, measured around execute_tests by nx::run.
/// A field the platform could not answer stays negative, and a report leaves it out rather than printing a zero.
struct nx::test_run_resources
{
    /// This process's CPU time over the run, in [0, 1] where 1 is every core busy — cc::process_cpu_load's scale.
    double cpu_machine_fraction = -1;

    /// The same load as a count of cores kept busy.
    double cpu_cores_used = -1;

    /// The OS's own high-water mark for this process's resident memory, so a spike between samples is not missed.
    i64 peak_resident_bytes = -1;
};

namespace nx
{

test_schedule_execution execute_tests(test_schedule const& schedule, test_schedule_config const& config);

} // namespace nx

namespace nx::impl
{
/// One run of a schedule, driven in steps rather than to completion.
///
/// `step` drives every test it can and returns once the run finishes or nothing here can progress, instead of parking the thread.
/// That is what a host needs whose results arrive only after the current call returns — a browser, or node and deno under Emscripten.
/// Every test runs one after another on the calling thread, as a `-j1` run does.
/// Between steps the run's scheduler stays bound to the calling thread, so work completing in between lands where the next step drives it.
///
/// `schedule` and `config` must outlive the run.
class test_run
{
public:
    test_run(test_schedule const& schedule, test_schedule_config const& config);
    ~test_run();

    test_run(test_run const&) = delete;
    test_run& operator=(test_run const&) = delete;

    /// Drives the run until it finishes (true) or nothing here can progress (false).
    [[nodiscard]] bool step();

    /// The run's result, once `step` has returned true.
    [[nodiscard]] test_schedule_execution take_result();

private:
    struct state;
    cc::unique_ptr<state> _state;
};
} // namespace nx::impl

namespace nx::impl
{
// Runs one test body through the section-replay loop under a freshly pushed, possibly nested context, finalizing stats into `execution.root`.
// `body` is invoked once per section-exploration pass.
// Shared by the top-level scheduler and by nx::invoke_tests, which runs parametrized-test bodies as nested executions.
//
// `filter_offset` shifts which scope element the context's first section level matches against.
// It is 0 at top level, and deeper for a dispatched child whose path already consumed leading segments.
// `section_scopes` is the effective set of allowed section paths for this instance.
// That is the grouped alias fragments' paths, or the run-global config.section_filters as a single scope.
// A section or dispatch runs if it matches ANY scope, and a dispatched child passes down the reduced subset consistent with its path.
void run_test_body(nx::test_execution& execution,
                   nx::test_schedule_config const& config,
                   cc::function_ref<void()> body,
                   cc::span<cc::vector<cc::string> const> section_scopes,
                   int filter_offset);

// Accessors into the innermost running test context, used by nx::invoke_tests.
// Must be called from within a running test body, so the context stack is non-empty.
nx::test_execution* current_execution(); // where dispatched children attach
nx::test_schedule_config const* current_config();
int current_filter_consumed(); // scope segments already matched by this path + ancestors
cc::span<cc::vector<cc::string> const> current_section_scopes(); // effective section scopes of the running instance

// Registry nx::invoke_tests queries for the test running here (nullptr outside a test).
// Read off the running instance through the ambient chain rather than a thread-local, so it is correct for a test running on any thread and for one dispatched from another.
nx::test_registry const* active_registry();

// The declaration of the SCHEDULED test the code here runs inside: the running test, or the top-level test that dispatched it.
// A dispatched child occupies that test's slot in the schedule, so this is whose config says what the child is actually run under.
// Null outside a test.
nx::test_declaration const* current_slot_declaration();

// True if `decl` is already running on the current execution chain (an ancestor invoke, or the running test
// itself). nx::invoke_tests uses this to break invocation cycles rather than recurse forever.
bool is_declaration_active(nx::test_declaration const* decl);

// Records an "invocation cycle" error on the currently running test (so it fails), naming `decl`. Called when
// a driver/invocable would (transitively) invoke a test already running above it.
void report_invocation_cycle(nx::test_declaration const* decl);
} // namespace nx::impl

namespace nx::impl
{
// Everything one CHECK/REQUIRE evaluation reports.
// The three sources of text are separate fields on purpose.
// Rendering picks the operands or the diagnostic per op, and ALWAYS appends the user annotations.
// So a chained .context() can never be shadowed by one of the framework's own strings.
struct check_result
{
    check_kind kind;
    cmp_op op;
    cc::string expr;
    bool passed = false;

    bool operands_captured = false; // lhs/rhs hold the decomposed comparison
    cc::string lhs;
    cc::string rhs;

    cc::string diagnostic; // the framework's own explanation (throws / asserts / a failing CC_ASSERT)

    cc::vector<cc::string> extra_lines; // user annotations only, in chaining order

    cc::source_location location;
};

void report_check_result(check_result result);

// Appends a metric to the active test's execution, and is a no-op when no test is running.
// nx::pgo is the public face.
void record_metric(cc::string_view name, double value, cc::rec::unit const& unit);

// Appends a measured loop to the active test's execution, and is a no-op when no test is running — which is what lets
// nx::bench::run be called from a plain function, or from an application, and simply hand its result back.
//
// **A result carrying an error-severity warning fails the test here.**
// A body that was optimized away has produced no number at all, and a benchmark reporting one would be worse than a
// benchmark that failed.
void record_benchmark_result(bench::result result);

// Crash-context hook (cc::crash_context_hook): writes the currently running test and section index to stderr.
// Registered with cc::add_crash_context_hook, so a fatal fault points at the offending test.
// Reads only plain globals updated per test, so it is safe to call from a constrained crash context.
void report_running_test() noexcept;
} // namespace nx::impl
