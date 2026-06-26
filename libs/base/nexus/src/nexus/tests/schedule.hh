#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/registry.hh>

namespace nx
{
struct test_instance
{
    test_declaration const* declaration = nullptr;
};

struct test_schedule_config
{
    cc::vector<cc::string> filters;
    cc::vector<cc::string> section_filters;
    bool run_disabled_tests = false;

    // run_manual_tests: manual tests become eligible (set by a non-wildcard filter, same as disabled, or by
    // --manual). only_manual_tests: --manual mode — the eligible set is *restricted* to manual tests, so
    // wildcard filters select among manual tests only and disabled tests stay excluded.
    bool run_manual_tests = false;
    bool only_manual_tests = false;
    bool is_catch2_xml_discovery = false;
    bool report_catch2_xml_results = false;
    bool verbose = false;

    // When non-empty, run() writes a JUnit XML report to this file path (in
    // addition to the normal console output). Set via --junit-xml <file>.
    cc::string junit_xml_file;

    static test_schedule_config create_from_args(int argc, char** argv);
};

struct test_schedule
{
    cc::vector<test_instance> instances;

    static test_schedule create(test_schedule_config const& config, test_registry const& registry);

    void print() const;
};

} // namespace nx
