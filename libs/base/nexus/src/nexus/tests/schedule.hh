#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/registry.hh>

namespace nx
{
struct test_instance
{
    test_declaration const* declaration = nullptr;

    // Section scopes for this instance: a set of allowed section paths. A section or dispatched invocable runs
    // if it matches ANY scope. Empty ⇒ fall back to the run-global config.section_filters. Every matched alias
    // fragment that shares a driver is grouped into one instance's scope set, so the driver body runs exactly
    // once no matter how many of its aliases matched — aliases are pure filters, not additive schedule entries.
    cc::vector<cc::vector<cc::string>> section_scopes;
};

struct test_schedule_config
{
    cc::vector<cc::string> filters;
    cc::vector<cc::string> section_filters;
    bool run_disabled_tests = false;

    // selected_bucket: the bucket an automatic sweep selects (normal by default, manual via --manual,
    // guide_benchmark via --guide-benchmarks). allow_cross_bucket_naming: no explicit bucket flag was given, so
    // a filter naming a test *exactly* may still pull it in from another bucket — checked per-test in
    // is_eligible. Neither bucket nor disabled status ever opens up to a bare substring filter: a test outside
    // selected_bucket, or a disabled one, needs its exact name (or its enabling flag / run_disabled_tests).
    nx::config::test_bucket selected_bucket = nx::config::test_bucket::normal;
    bool allow_cross_bucket_naming = false;
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

    // True if some non-empty filter equals the test name *exactly* (not a substring). An exact name is
    // what pulls in an otherwise-excluded disabled test, or one from another bucket; a substring filter
    // does not. Always false when filters is empty.
    bool name_matches_exact(test_declaration const& decl) const;

    // Bucket + disabled eligibility for `decl`, given whether the filter that selected it named its target
    // *exactly* — the test's own name for a direct match, the alias name for an alias fragment. A test
    // outside selected_bucket, or a disabled one, is eligible only on that exact name (or its enabling
    // flag: a bucket flag / run_disabled_tests). Filters are not applied here; see would_run.
    bool is_eligible(test_declaration const& decl, bool named_exactly) const;

    // True if the test would be scheduled under this config: is_eligible() for its bucket/disabled status
    // AND name_matches(). This is exactly the predicate test_schedule::create uses for a directly named
    // test (aliases route through is_eligible with the alias name as the key).
    bool would_run(test_declaration const& decl) const;

    // True if some non-empty filter is a substring of the alias name. Always false when filters is empty: a
    // full sweep already runs every driver unscoped (invoking every invocable), so expanding aliases too
    // would double-run them. Aliases therefore only take effect under an explicit filter.
    bool alias_matches(test_alias const& alias) const;

    // True if some non-empty filter equals the alias name *exactly*. An exact alias name is what pulls its
    // fragments' drivers in across a bucket, or enables a disabled driver — a substring filter does not.
    // Always false when filters is empty.
    bool alias_matches_exact(test_alias const& alias) const;

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
