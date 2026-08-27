#include "run.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/args/ambient.hh>
#include <nexus/bench/report.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/catch2.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/listing_json.hh>
#include <nexus/tests/export/pgo_json.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <unordered_set> // std::unordered_set: cc has no set type yet

namespace
{
// The JUnit suite name.
// nx::program_name does the basename-and-strip-.exe work against the argv nx::run captured, and "nexus"
// stands in on a platform that could not say what we are called.
cc::string suite_name()
{
    auto const name = nx::program_name();
    return name.empty() ? cc::string("nexus") : cc::string(name);
}

// Writes a report file whole, replacing any existing content.
cc::result<cc::unit> write_report_file(cc::string_view path, cc::string_view content)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);
    auto stream = adapter.value().stream();
    CC_RETURN_IF_ERROR(stream.write(cc::as_bytes(content)));
    CC_RETURN_IF_ERROR(stream.flush()); // no auto-flush: buffered bytes are lost otherwise
    return cc::unit{};
}

// Prints failing tests to stderr, recursing into invoked (nested) executions.
// `prefix` is the parent's accumulated addressable path — invocation group plus name segments — so a failing instance shows its full "driver / group / test" location.
void print_failing(nx::test_execution const& exec, cc::string const& prefix)
{
    auto const* const decl = exec.instance.declaration;

    cc::string label = prefix;
    if (!exec.invocation_group.empty())
    {
        if (!label.empty())
            label += " / ";
        label += exec.invocation_group;
    }
    if (decl != nullptr)
    {
        if (!label.empty())
            label += " / ";
        label += decl->name;
    }

    if (exec.root.is_considered_failing && decl != nullptr)
    {
        cc::eprintln("  {} at {}:{}", label, decl->location.file_name(), decl->location.line());

        // What it ran WITH, when that is not simply nothing: a parametrized example that failed is not
        // reproducible from its name alone.
        if (!exec.instance.args.empty())
        {
            auto line = cc::string();
            for (auto const& arg : exec.instance.args)
            {
                if (!line.empty())
                    line += " ";

                line += arg;
            }

            cc::eprintln("    with args: {}", line);
        }
    }

    for (auto const& child : exec.nested)
        print_failing(child, label);
}

// Collects the declarations of every invoked (nested) execution, which is the set of invocable tests that actually ran this session.
// Used to detect orphans: declared but never invoked.
void collect_invoked(nx::test_execution const& exec, std::unordered_set<void const*>& out)
{
    for (auto const& child : exec.nested)
    {
        out.insert(child.instance.declaration);
        collect_invoked(child, out);
    }
}

/// The directory part of `path`, or empty when it has none.
cc::string_view directory_of(cc::string_view path)
{
    for (auto i = path.size(); i > 0; --i)
    {
        auto const c = path[i - 1];
        if (c == '/' || c == '\\')
            return path.subview({.offset = 0, .size = i - 1});
    }
    return {};
}
} // namespace

int nx::run(int argc, char** argv)
{
    // Before anything else can start a thread: a test asking for nx::main_thread means THIS one.
    cc::mark_current_thread_as_main();

    // Record the command line so nx::test_args can answer from anywhere, including a library deep in a
    // call stack that has no argv of its own.
    nx::impl::set_process_args(argc, argv);

    // Install a crash handler so a fatal fault in a test prints the offending test and a
    // stacktrace instead of a bare non-zero exit code.
    cc::install_crash_handler();
    cc::add_crash_context_hook(&nx::impl::report_running_test);

    // Create schedule config from command line arguments
    auto config = test_schedule_config::create_from_args(argc, argv);

    // Help is generated from the same declaration the parse uses, so it cannot describe a flag nexus lacks,
    // and the PARSE is what says it was asked for.
    // Scanning argv instead would claim a --help that belongs to the test — one carried by --test-args, or
    // sitting past a bare --.
    // The "Compatible with Catch2" line it carries is what makes C++ TestMate recognize this binary at all.
    if (config.help_requested)
    {
        cc::println(test_schedule_config::cli_help_text());
        return 0;
    }

    // A command line that did not parse stops here: the parse already reported what was wrong, and running
    // the subset it managed to understand is the one outcome a mistyped flag must never produce.
    if (config.parse_failed)
        return 1;

    // Get the static test registry
    auto& registry = get_static_test_registry();

    // Run NX_TEST_SETUP callbacks: they define aliases (with full registry access) and must run before any
    // listing or scheduling, so aliases are visible even when we only list/discover tests and never run them.
    nx::run_setup_callbacks(registry);

    // Settle name-vs-file matching once, before anything queries a filter: the listing below and the schedule must agree.
    // Aliases are registered by then, so a filter naming one counts as a name match and suppresses the file fallback.
    config.resolve_filter_mode(registry);

    // Handle Catch2 XML discovery mode for TestMate integration
    if (config.is_catch2_xml_discovery)
    {
        cc::print(write_catch2_discovery_xml(registry));
        return 0;
    }

    // JSON test listing: the query `dev.py test` uses to pre-select which binaries actually contain a matching test.
    // It reports every registered test plus its eligibility under the parsed args, and never runs anything.
    // It always succeeds, even when nothing is eligible — the caller decides what an empty match means.
    if (!config.list_tests_json_file.empty())
    {
        auto const json = write_test_listing_json(suite_name(), config, registry);
        if (config.list_tests_json_file == "-")
            cc::print(json);
        else if (auto const written = write_report_file(config.list_tests_json_file, json); !written.has_value())
        {
            cc::eprintln("Error: could not write test listing JSON file: {}: {}", config.list_tests_json_file,
                         written.error().to_string());
            return 1;
        }
        return 0;
    }

    // Create schedule from config and registry
    auto schedule = test_schedule::create(config, registry);

    // Check if any tests were scheduled
    if (schedule.instances.empty())
    {
        // A pgo-benchmark sweep over a binary that has none is not an error: `dev.py pgo` runs
        // --pgo-benchmarks across every test binary, and most contain no PGO benchmarks.
        if (config.selected_bucket == nx::config::test_bucket::pgo_benchmark)
        {
            cc::println("No PGO benchmarks in this binary");
            return 0;
        }

        // Same for benchmarks: `dev.py benchmark` probes every binary to resolve a name, and most carry none.
        if (config.selected_bucket == nx::config::test_bucket::benchmark)
        {
            cc::println("No benchmarks in this binary");
            return 0;
        }

        // Same for examples: `dev.py example` probes every binary to resolve a name, and most carry none.
        if (config.selected_bucket == nx::config::test_bucket::example)
        {
            cc::println("No examples in this binary");
            return 0;
        }

        cc::eprintln("Error: The current schedule did not select any tests");
        for (int i = 0; i < argc; ++i)
            cc::eprintln("  arg[{}] = `{}'", i, argv[i]);
        return 1;
    }

    if (config.verbose)
    {
        schedule.print();
        cc::println();
    }

    // Stand the recorder up for the WHOLE run, never per test.
    // Per-test attribution rides the ambient chain instead, so a test that records nothing costs nothing, and a test
    // asking what it recorded gets an answer without anyone parsing history back to the start of the process.
    if (!config.no_recording)
        nx::impl::begin_run_recording();

    // Execute the scheduled tests
    auto execution = execute_tests(schedule, config);

    // A failing test's recording is written beside the run's other artifacts, which is why this follows the JUnit
    // file's directory rather than inventing a location of its own.
    nx::impl::end_run_recording(directory_of(config.junit_xml_file));

    // Write a JUnit XML report if requested.
    // This is additive: the console output below still runs, whatever the reporting mode.
    if (!config.junit_xml_file.empty())
    {
        auto const written = write_report_file(config.junit_xml_file, write_junit_xml(suite_name(), execution));
        if (!written.has_value())
            cc::eprintln("Error: could not write JUnit XML file: {}: {}", config.junit_xml_file,
                         written.error().to_string());
    }

    // Write a perf-metrics JSON sidecar if requested (the metrics recorded via nx::pgo). Also additive.
    if (!config.pgo_json_file.empty())
    {
        auto const written = write_report_file(config.pgo_json_file, write_pgo_json(suite_name(), execution));
        if (!written.has_value())
            cc::eprintln("Error: could not write perf JSON file: {}: {}", config.pgo_json_file,
                         written.error().to_string());
    }

    // Handle Catch2 XML results reporting for TestMate integration
    if (config.report_catch2_xml_results)
    {
        cc::print(write_catch2_results_xml(execution));
        return execution.count_failed_tests() > 0 ? 1 : 0;
    }

    // Print what the benchmarks measured.
    //
    // One report per BENCHMARK rather than one for the whole run: the loops inside one body are what get compared, and
    // a table spanning two benchmarks would invite a comparison between numbers measured minutes apart.
    {
        auto const style = nx::bench::report_style::for_console();
        for (auto const& exec : execution.executions)
        {
            if (exec.benchmarks.empty())
                continue;

            cc::println();
            cc::print(nx::bench::format_report(exec.instance.declaration->name, exec.benchmarks, style));
        }
    }

    // Print any metrics recorded via nx::pgo (PGO benchmarks). Console-only mirror of the perf JSON sidecar.
    {
        bool has_metrics = false;
        for (auto const& exec : execution.executions)
            if (!exec.metrics.empty())
            {
                has_metrics = true;
                break;
            }

        if (has_metrics)
        {
            cc::println("\nRecorded metrics:");
            for (auto const& exec : execution.executions)
                for (auto const& metric : exec.metrics)
                {
                    char const* const dir = metric.higher_is_better ? "(higher is better)" : "(lower is better)";
                    cc::println("  {} | {} = {} {} {}", exec.instance.declaration->name, metric.name, metric.value,
                                metric.unit, dir);
                }
        }
    }

    // Orphan invocable tests: in a full, unfiltered normal sweep every enabled INVOCABLE_TEST must be
    // invoked by some driver (see nx::invoke_tests). Anything left over is almost always a wiring mistake.
    int orphan_count = 0;
    bool const full_normal_sweep = config.filters.empty() && config.section_filters.empty()
                                && config.selected_bucket == nx::config::test_bucket::normal;
    if (full_normal_sweep)
    {
        std::unordered_set<void const*> invoked;
        for (auto const& exec : execution.executions)
            collect_invoked(exec, invoked);

        for (auto const& decl : registry.declarations)
            if (decl.is_invocable() && decl.test_config.enabled && !invoked.contains(&decl))
            {
                if (orphan_count == 0)
                    cc::eprintln("\nOrphan invocable tests (declared but never invoked):");
                cc::eprintln("  {} at {}:{}", decl.name, decl.location.file_name(), decl.location.line());
                ++orphan_count;
            }
    }

    // Check for failures
    int const failed_tests = execution.count_failed_tests();
    int const total_tests = execution.count_total_tests();
    int const failed_checks = execution.count_failed_checks();
    int const total_checks = execution.count_total_checks();

    // A check that could not be attributed to any test proved nothing, so it fails the run — however green every test is.
    // Each one was already printed where it happened; this is the summary that makes the run's exit code say so.
    int const orphan_checks = execution.orphan_checks;

    if (failed_tests > 0 || orphan_count > 0 || orphan_checks > 0)
    {
        if (failed_tests > 0)
        {
            cc::eprintln("\nFailed tests:");
            for (auto const& exec : execution.executions)
                print_failing(exec, cc::string());

            cc::eprintln("\n{} of {} tests failed", failed_tests, total_tests);
            cc::eprintln("Failed {} of {} checks", failed_checks, total_checks);
        }
        if (orphan_count > 0)
            cc::eprintln("\n{} invocable test(s) were never invoked", orphan_count);
        if (orphan_checks > 0)
        {
            cc::eprintln("\nChecks outside any test:");
            for (auto const& e : execution.orphan_errors)
                cc::eprintln("  {} at {}:{}", e.expanded, e.location.file_name(), e.location.line());
            cc::eprintln("\n{} check(s) ran outside any test context", orphan_checks);
        }
        return 1;
    }

    // All tests passed
    cc::println("All {} tests passed ({} checks)", total_tests, total_checks);
    return 0;
}
