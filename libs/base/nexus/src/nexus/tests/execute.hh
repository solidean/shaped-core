#pragma once

#include <nexus/tests/schedule.hh>

#include <source_location>
#include <string>
#include <vector>


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
    std::string expr; // usually not shown in editor (assumed to be part of source already)
    std::source_location location;
    std::vector<std::string> extra_lines;
    std::string expanded; // shown inline next to the code at location (important for VSCode DX)
    // NOTE: if expr == expanded, C++ TestMate just shows "failed" instead of anything useful, so make sure they are always different
};

struct test_execution
{
    test_instance instance;

    struct section
    {
        std::string name;
        std::source_location location;
        std::vector<section> subsections;

        // NOTE: only valid for leaf sections
        std::vector<test_error> errors;

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
    std::vector<test_execution> executions;

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
                         std::string expr,
                         bool passed,
                         std::vector<std::string> extra_lines,
                         std::source_location location);
}
