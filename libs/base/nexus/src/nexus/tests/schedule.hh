#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/registry.hh>

struct nx::test_instance
{
    test_declaration const* declaration = nullptr;

    // The registry this declaration came from, and the one nx::invoke_tests searches from inside its body.
    // Carried per instance rather than per run: tests execute concurrently, so "the registry of the current run" cannot live in a thread-local.
    test_registry const* registry = nullptr;

    // Section scopes for this instance: a set of allowed section paths, and a section or dispatched invocable runs if it matches ANY of them.
    // Empty means fall back to the run-global config.section_filters.
    // Every matched alias fragment sharing a driver is grouped into one instance's scope set, so the driver body runs exactly once however many of its aliases matched.
    // Aliases are pure filters, not additive schedule entries.
    cc::vector<cc::vector<cc::string>> section_scopes;
};

// How the positional filters are read.
// A file filter is a glob over the test's source file — the path the compiler saw, so an absolute one works too.
// It selects like a name filter does: bucket and disabled rules still apply, because only an *exact test name* opens those.
enum class nx::filter_mode
{
    name,
    file,
    name_or_file, // names first, and file globs only when no filter matched any name
};

struct nx::test_schedule_config
{
    cc::vector<cc::string> filters;
    cc::vector<cc::string> section_filters;
    bool run_disabled_tests = false;

    // How to read `filters`; --match-files / --match-names pin it, and name_or_file is the default.
    filter_mode mode = filter_mode::name_or_file;

    // The resolved answer for name_or_file, written only by resolve_filter_mode.
    // A config that never resolves stays on names, so an embedder building one by hand keeps today's behavior.
    bool matching_files = false;

    // selected_bucket is the bucket an automatic sweep selects; nx::config::test_bucket documents the set.
    // allow_cross_bucket_naming records that no explicit bucket flag was given, so an exactly-named test may still be pulled in from another bucket.
    // is_eligible below applies both, and is the one place those rules live.
    nx::config::test_bucket selected_bucket = nx::config::test_bucket::normal;
    bool allow_cross_bucket_naming = false;
    // Upper bound on how many tests may run at once, --jobs N; 0 means the machine's hardware concurrency.
    // 1 drives them one at a time in schedule order, which is the reproducible-debugging mode rather than a pool of one.
    // It is the default HERE, for a hand-built config, so a test that builds a schedule gets the deterministic order unless it asks otherwise.
    // A real run goes the other way: create_from_args starts at 0, so `nx::run` with no --jobs uses every core.
    // A test asking for own_pool(n) names its own width instead, and is unaffected.
    int jobs = 1;

    bool is_catch2_xml_discovery = false;
    bool report_catch2_xml_results = false;
    bool verbose = false;

    // Bucket EVERY test, not just the ones asking for nx::config::recorded.
    //
    // A debug flag, and normally paired with a filter: bucketing the whole suite costs the worst binary in this repo
    // about a third of its wall time, and nothing releases a bucket until its test passes.
    // Retention is deliberately unbounded — you asked for the whole recording, so you get the whole recording.
    bool record_all = false;

    // Skip the run's cc::rec recorder entirely — no per-test buckets, no console listener, no dumps.
    // Recording costs a run roughly a quarter to a third of its wall time (see libs/base/nexus/docs/recording.md), which is
    // worth paying to be able to ask what a test recorded, and worth skipping when timing the tests themselves.
    bool no_recording = false;

    // When non-empty, run() writes a JUnit XML report to this path, additionally to the normal console output.
    // Set via --junit-xml <file>.
    cc::string junit_xml_file;

    // When non-empty, run() writes a perf-metrics JSON sidecar to this path, additionally to the console output.
    // Set via --perf-json <file>; nx::guide records the metrics.
    cc::string perf_json_file;

    // The arguments the selected test itself receives, reachable from its body through nx::current_args().
    // Set via --test-args "<line>", or by everything after a bare --.
    // Tokenized once here, by the same rules a response file uses.
    // Recorded but not yet delivered: wiring it through the scheduler is a separate change.
    cc::vector<cc::string> test_args;

    // When non-empty, run() writes a JSON test listing to this path ("-" means stdout) and exits without running anything.
    // Set via --list-tests-json <file>.
    // The listing reports every registered test plus whether it would_run() under the rest of the parsed args, so a caller can pre-select binaries.
    cc::string list_tests_json_file;

    // True if the test passes the filters alone: filters empty, or some non-empty filter matches.
    // A filter matches as a substring of the test name, or — once resolve_filter_mode selected files — as a glob over its source file.
    // Bucket and disabled status are ignored, which is what distinguishes "filter didn't match" from "matched but excluded".
    bool filter_matches(test_declaration const& decl) const;

    // True if some non-empty filter equals the test name *exactly*, never as a substring.
    // An exact name is what pulls in an otherwise-excluded disabled test, or one from another bucket.
    // Names only, whatever the filter mode: a file glob never unlocks a disabled or out-of-bucket test.
    // Always false when filters is empty.
    bool name_matches_exact(test_declaration const& decl) const;

    // Bucket and disabled eligibility for `decl`, given whether the filter that selected it named its target *exactly*.
    // That target is the test's own name for a direct match, and the alias name for an alias fragment.
    // A test outside selected_bucket, or a disabled one, is eligible only on that exact name, or on its enabling flag — a bucket flag, or run_disabled_tests.
    // Filters are not applied here; would_run combines the two.
    bool is_eligible(test_declaration const& decl, bool named_exactly) const;

    // True if the test would be scheduled under this config: is_eligible() for its bucket and disabled status, AND filter_matches().
    // This is exactly the predicate test_schedule::create uses for a directly named test.
    // An alias routes through is_eligible with the alias name as the key instead.
    bool would_run(test_declaration const& decl) const;

    // True if some non-empty filter is a substring of the alias name — or a glob over the alias' source file, in file mode.
    // Always false when filters is empty: a full sweep already runs every driver unscoped, invoking every invocable, so expanding aliases too would double-run them.
    // An alias therefore only takes effect under an explicit filter.
    bool alias_filter_matches(test_alias const& alias) const;

    // True if some non-empty filter equals the alias name *exactly*.
    // An exact alias name is what pulls its fragments' drivers in across a bucket, or enables a disabled driver; a substring filter does not.
    // Always false when filters is empty.
    bool alias_matches_exact(test_alias const& alias) const;

    // Settles filter_mode::name_or_file against `registry`: when no filter matches any test or alias *name*, the filters are re-read as file globs.
    // Run it once, after create_from_args and before the first filter query — the schedule and the JSON listing must see the same answer.
    // A no-op for the pinned modes, and for an empty filter set.
    void resolve_filter_mode(test_registry const& registry);

    static test_schedule_config create_from_args(int argc, char** argv);

    /// The --help page, generated from the same declaration create_from_args parses with.
    /// Shared rather than hand-written, so help cannot describe a flag the parser does not have.
    [[nodiscard]] static cc::string cli_help_text();
};

struct nx::test_schedule
{
    cc::vector<test_instance> instances;

    // The registry these instances came from.
    // nx::invoke_tests queries it to find parametrized tests, so a run against a local registry — in a meta-test, say — dispatches within that same registry.
    test_registry const* registry = nullptr;

    static test_schedule create(test_schedule_config const& config, test_registry const& registry);

    void print() const;
};
