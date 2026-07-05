#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/schedule.hh>


// Forward declaration for impl namespace
namespace nx::impl
{
enum class check_kind;
enum class cmp_op;
} // namespace nx::impl

namespace nx
{
struct test_error
{
    cc::string expr; // usually not shown in editor (assumed to be part of source already)
    cc::source_location location;
    cc::vector<cc::string> extra_lines;
    cc::string expanded; // shown inline next to the code at location (important for VSCode DX)
    // NOTE: if expr == expanded, C++ TestMate just shows "failed" instead of anything useful, so make sure they are always different
};

// A single performance metric recorded by a guide benchmark via nx::guide (see guide.hh).
// higher_is_better orients comparisons (throughput vs. latency); unit is a free-form label (e.g. "GB/s", "s").
struct recorded_metric
{
    cc::string name;
    double value = 0;
    cc::string unit;
    bool higher_is_better = true;
};

struct test_execution
{
    test_instance instance;

    // Metrics recorded by nx::guide during this test (typically a guide benchmark). Empty for normal tests.
    cc::vector<recorded_metric> metrics;

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

    // Executions dispatched from this test's body via nx::invoke_tests (parametrized-test instances run as
    // addressable children). Empty for ordinary tests. invocation_group is the nx::invoke_tests(name) segment
    // under which this execution ran (empty for a top-level test); the child's own name is
    // instance.declaration->name, so its addressable path is invocation_group / declaration name / sections.
    cc::vector<test_execution> nested;
    cc::string invocation_group;

    // Failing if this test's own tree fails or any dispatched child fails.
    [[nodiscard]] bool is_considered_failing() const;
};

struct test_schedule_execution
{
    cc::vector<test_execution> executions;

    // All counts recurse into dispatched (nested) executions: a dispatched instance counts as its own test.
    [[nodiscard]] int count_total_tests() const;
    [[nodiscard]] int count_failed_tests() const;
    [[nodiscard]] int count_total_checks() const;
    [[nodiscard]] int count_failed_checks() const;
};

test_schedule_execution execute_tests(test_schedule const& schedule, test_schedule_config const& config);

} // namespace nx

namespace nx::impl
{
// Runs one test body through the section-replay loop under a freshly pushed (possibly nested) context,
// finalizing stats into `execution.root`. `body` is invoked once per section-exploration pass. Shared by
// the top-level scheduler and nx::invoke_tests (which runs parametrized-test bodies as nested executions).
// `filter_offset` shifts which section_filters element the context's first section level matches against
// (0 at top level; deeper for dispatched children whose path already consumed leading filter segments).
// `section_filters` is the effective per-instance section scope (an alias fragment's path, or the run-global
// config.section_filters); a dispatched child passes down its parent's span so scoping stays consistent.
void run_test_body(nx::test_execution& execution,
                   nx::test_schedule_config const& config,
                   cc::function_ref<void()> body,
                   cc::span<cc::string const> section_filters,
                   int filter_offset);

// Accessors into the innermost running test context, used by nx::invoke_tests. Must be called from within a
// running test body (the context stack is non-empty).
nx::test_execution* current_execution(); // where dispatched children attach
nx::test_schedule_config const* current_config();
int current_filter_consumed();                        // section_filters already matched by this path + ancestors
cc::span<cc::string const> current_section_filters(); // effective section scope of the running instance

// Registry nx::invoke_tests queries during the active execute_tests run (nullptr outside a run). Set from the
// running schedule, so dispatching within a local-registry run stays within that registry.
nx::test_registry const* active_registry();
} // namespace nx::impl

namespace nx::impl
{
void report_check_result(check_kind kind,
                         cmp_op op,
                         cc::string expr,
                         bool passed,
                         cc::vector<cc::string> extra_lines,
                         cc::source_location location);

// Appends a metric to the active test's execution. No-op when no test is running. Used by nx::guide.
void record_metric(cc::string_view name, double value, cc::string_view unit, bool higher_is_better);

// Crash-context hook (cc::crash_context_hook): writes the currently running test (and section index)
// to stderr. Registered with cc::add_crash_context_hook so a fatal fault points at the offending test.
// Reads only plain globals updated per test; safe to call from a constrained crash context.
void report_running_test() noexcept;
} // namespace nx::impl
