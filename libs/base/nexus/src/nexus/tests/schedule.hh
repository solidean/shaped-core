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

    // When non-empty, run() writes a JSON test listing to this path (or stdout if
    // "-") and exits without running anything. Set via --list-tests-json <file>.
    // The listing reports every registered test plus whether it would_run() under
    // the rest of the parsed args, so callers can pre-select binaries to run.
    cc::string list_tests_json_file;

    // True if the test's name passes the filters alone (filters empty, or some
    // non-empty filter is a substring of the name) — bucket and disabled status
    // are ignored. Distinguishes "name didn't match" from "matched but excluded".
    bool name_matches(test_declaration const& decl) const;

    // True if the test would be scheduled under this config: name_matches() AND the
    // bucket gate (match_any_bucket, or same bucket) AND the disabled gate (enabled,
    // or run_disabled_tests). This is exactly the predicate test_schedule::create uses.
    bool would_run(test_declaration const& decl) const;

    static test_schedule_config create_from_args(int argc, char** argv);
};

struct test_schedule
{
    cc::vector<test_instance> instances;

    // The registry these instances came from. nx::invoke_tests queries it to find parametrized tests to run,
    // so a run against a local registry (e.g. in tests) dispatches within that same registry.
    test_registry const* registry = nullptr;

    static test_schedule create(test_schedule_config const& config, test_registry const& registry);

    void print() const;
};

} // namespace nx
