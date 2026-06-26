#pragma once

#include <clean-core/container/vector.hh>
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

    [[nodiscard]] bool is_considered_failing() const;
};

struct test_schedule_execution
{
    cc::vector<test_execution> executions;

    [[nodiscard]] int count_total_tests() const;
    [[nodiscard]] int count_failed_tests() const;
    [[nodiscard]] int count_total_checks() const;
    [[nodiscard]] int count_failed_checks() const;
};

test_schedule_execution execute_tests(test_schedule const& schedule, test_schedule_config const& config);

} // namespace nx

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
} // namespace nx::impl
