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

    // selected_bucket: the bucket an automatic sweep selects (normal by default, manual via --manual,
    // guide_benchmark via --guide-benchmarks). match_any_bucket: a non-wildcard filter was given without an
    // explicit bucket flag, so the sweep is not restricted to selected_bucket — an explicitly named test runs
    // regardless of its bucket (disabled ones too, via run_disabled_tests).
    nx::config::test_bucket selected_bucket = nx::config::test_bucket::normal;
    bool match_any_bucket = false;
    bool is_catch2_xml_discovery = false;
    bool report_catch2_xml_results = false;
    bool verbose = false;

    // When non-empty, run() writes a JUnit XML report to this file path (in
    // addition to the normal console output). Set via --junit-xml <file>.
    cc::string junit_xml_file;

    // When non-empty, run() writes a perf-metrics JSON sidecar to this path (in
    // addition to console output). Set via --perf-json <file>. See nx::guide.
    cc::string perf_json_file;

    static test_schedule_config create_from_args(int argc, char** argv);
};

struct test_schedule
{
    cc::vector<test_instance> instances;

    static test_schedule create(test_schedule_config const& config, test_registry const& registry);

    void print() const;
};

} // namespace nx
