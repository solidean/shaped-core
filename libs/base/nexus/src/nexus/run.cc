#include "run.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/platform/process_metrics.hh>
#include <clean-core/record/crash_dump.hh>
#include <clean-core/record/quantity_format.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/args/ambient.hh>
#include <nexus/bench/environment.hh>
#include <nexus/bench/report.hh>
#include <nexus/impl/hang_watchdog.hh>
#include <nexus/impl/host_loop.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/entry.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/bench_json.hh>
#include <nexus/tests/export/catch2.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/listing_json.hh>
#include <nexus/tests/export/pgo_json.hh>
#include <nexus/tests/export/timings_json.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <unordered_set> // std::unordered_set: keyed on declaration addresses, where cc::set's hashing requirements do not apply

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

/// "37% avg cpu load (11.8 cores), 1.42 GiB peak ram", or what of it the platform could measure, which may be nothing.
cc::string describe_resources(nx::test_run_resources const& r)
{
    auto out = cc::string();
    if (r.cpu_machine_fraction >= 0)
        out.appendf("{:.0f}% avg cpu load ({:.1f} cores)", r.cpu_machine_fraction * 100, r.cpu_cores_used);
    if (r.peak_resident_bytes >= 0)
    {
        if (!out.empty())
            out += ", ";
        cc::rec::format_quantity_to(out, double(r.peak_resident_bytes), cc::rec::unit_bytes);
        out += " peak ram";
    }
    return out;
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
/// Everything the report after a run needs from before it.
struct run_reporting
{
    nx::test_registry const* registry = nullptr;
    bool is_entry_run = false;
    bool has_benchmarks = false;
    nx::bench::load_sample benchmark_load_before;
    bool benchmark_pinned = false;
    cc::unique_ptr<cc::process_cpu_sampler> cpu_sampler;
};

/// Writes every report a run asked for and returns the process's exit code.
int report_run(nx::test_schedule_config const& config,
               run_reporting& reporting,
               nx::test_schedule_execution const& execution)
{
    auto const& registry = *reporting.registry;
    auto const is_entry_run = reporting.is_entry_run;
    auto const has_benchmarks = reporting.has_benchmarks;
    auto const& benchmark_load_before = reporting.benchmark_load_before;
    auto const benchmark_pinned = reporting.benchmark_pinned;
    auto& cpu_sampler = *reporting.cpu_sampler;

    auto resources = nx::test_run_resources{};
    if (auto const load = cpu_sampler.sample(); load.has_value())
    {
        resources.cpu_machine_fraction = load.value().machine_fraction;
        resources.cpu_cores_used = load.value().cores_used;
    }
    if (auto const usage = cc::query_process_usage(); usage.has_value())
        resources.peak_resident_bytes = usage.value().peak_resident_bytes;

    // An example is a program someone is watching rather than a suite being measured, so only tests say what they cost.
    auto const reports_resources = config.selected_bucket != nx::config::test_bucket::example && !is_entry_run;

    // A failing test's recording is written beside the run's other artifacts, which is why this follows the JUnit
    // file's directory rather than inventing a location of its own.
    nx::impl::end_run_recording(directory_of(config.junit_xml_file));

    // Write a JUnit XML report if requested.
    // This is additive: the console output below still runs, whatever the reporting mode.
    if (!config.junit_xml_file.empty())
    {
        auto const written = write_report_file(
            config.junit_xml_file,
            write_junit_xml(suite_name(), execution, resources,
                            config.shuffle ? cc::optional<decltype(config.seed)>(config.seed) : cc::nullopt));
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

    // Additive as well: where each test sat on the timeline, for a trace of the run.
    if (!config.timings_json_file.empty())
    {
        auto const written = write_report_file(config.timings_json_file, write_timings_json(suite_name(), execution));
        if (!written.has_value())
            cc::eprintln("Error: could not write timings JSON file: {}: {}", config.timings_json_file,
                         written.error().to_string());
    }

    // Handle Catch2 XML results reporting for TestMate integration
    if (config.report_catch2_xml_results)
    {
        cc::print(write_catch2_results_xml(execution));
        return execution.count_failed_tests() > 0 ? 1 : 0;
    }

    if (has_benchmarks)
    {
        auto const after = nx::bench::sample_load();

        // A clock ratio that moved means the frequency changed or contention appeared, which is exactly the condition
        // worth warning about — and it measures the core the benchmark ran on rather than the machine as a whole.
        auto const drift
            = benchmark_load_before.ticks_per_ns > 0
                ? (after.ticks_per_ns - benchmark_load_before.ticks_per_ns) / benchmark_load_before.ticks_per_ns
                : 0.0;

        cc::println();
        cc::print(cc::format("load  clock {:.3f} -> {:.3f} ticks/ns ({:+.1f}%)", benchmark_load_before.ticks_per_ns,
                             after.ticks_per_ns, drift * 100));
        if (after.cpu_busy_fraction >= 0)
            cc::print(cc::format("  |  machine {:.0f}% busy", after.cpu_busy_fraction * 100));
        cc::println();

        if (benchmark_pinned)
            nx::bench::unpin();
    }

    // Write the benchmark sidecar if requested.
    // Additive, like the reports above it.
    if (!config.benchmark_json_file.empty())
    {
        auto const written = write_report_file(config.benchmark_json_file, write_bench_json(suite_name(), execution));
        if (!written.has_value())
            cc::eprintln("Error: could not write benchmark JSON file: {}: {}", config.benchmark_json_file,
                         written.error().to_string());
    }

    // Print what the benchmarks measured.
    //
    // One report per BENCHMARK rather than one for the whole run: the loops inside one body are what get compared, and
    // a table spanning two benchmarks would invite a comparison between numbers measured minutes apart.
    {
        auto style = nx::bench::report_style::for_console();
        style.verbose = config.benchmark_verbose;
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
                    // Formatted through the unit, so a byte rate reads as 27.1 GiB/s rather than as eleven digits.
                    // That is the payoff of a metric carrying a cc::rec::unit rather than a label.
                    char const* const dir = metric.higher_is_better() ? "(higher is better)" : "(lower is better)";
                    cc::println("  {} | {} = {} {}", exec.instance.declaration->name, metric.name,
                                nx::bench::format_quantity(metric.value, metric.unit), dir);
                }
        }
    }

    // Orphan invocable tests: in a full, unfiltered normal sweep every enabled INVOCABLE_TEST must be
    // invoked by some driver (see nx::invoke_tests). Anything left over is almost always a wiring mistake.
    //
    // An invocable an alias can reach is exempt, because the mistake this catches is "nothing can run this" rather than
    // "nothing ran this". A driver may be deliberately disabled — a backend still being built out registers so its
    // aliases exist, and disables so a sweep stays out of the parts it has not reached — and its invocables are then
    // parked rather than unwired, runnable by name whenever someone asks for one.
    int orphan_count = 0;
    bool const full_normal_sweep = config.filters.empty() && config.section_filters.empty()
                                && config.selected_bucket == nx::config::test_bucket::normal;
    if (full_normal_sweep)
    {
        std::unordered_set<void const*> invoked;
        for (auto const& exec : execution.executions)
            collect_invoked(exec, invoked);

        // Every invocable name some alias expands onto, which is the set an alias can drive by name.
        cc::set<cc::string_view> alias_reachable;
        for (auto const& alias : registry.aliases)
        {
            alias_reachable.insert(cc::string_view(alias.name));
            for (auto const& fragment : alias.fragments)
                for (auto const& section : fragment.section_path)
                    alias_reachable.insert(cc::string_view(section));
        }

        for (auto const& decl : registry.declarations)
            if (decl.is_invocable() && decl.test_config.enabled && !invoked.contains(&decl)
                && !alias_reachable.contains(cc::string_view(decl.name)))
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

    // A warning or error under no test is a defect for the same reason, and each was already printed when it was logged.
    auto const unattributed_logs = execution.unattributed_logs.size();

    if (failed_tests > 0 || orphan_count > 0 || orphan_checks > 0 || unattributed_logs > 0)
    {
        if (failed_tests > 0)
        {
            if (config.shuffle)
                cc::eprintln("\nrun seed {} — reproduce the order with --seed {}", config.seed, config.seed);
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
        if (unattributed_logs > 0)
        {
            cc::eprintln("\nWarnings and errors logged outside any test:");
            for (auto const& e : execution.unattributed_logs)
                cc::eprintln("  {} ({})", e.expanded, e.extra_lines.empty() ? cc::string() : e.extra_lines[0]);
            cc::eprintln("\n{} warning(s) or error(s) were logged outside any test", unattributed_logs);
        }
        if (auto const described = reports_resources ? describe_resources(resources) : cc::string(); !described.empty())
            cc::eprintln("{}", described);

        // A failed command keeps the status it chose, unless that status was success.
        if (is_entry_run && !execution.executions.empty())
            if (auto const code = execution.executions[0].exit_code.value_or(0); code != 0)
                return code;
        return 1;
    }

    // An app or a command is a program, not a suite: its status is its own, and it prints no test summary.
    if (is_entry_run)
        return execution.executions.empty() ? 0 : execution.executions[0].exit_code.value_or(0);

    // All tests passed
    cc::println("All {} tests passed ({} checks)", total_tests, total_checks);
    if (auto const described = reports_resources ? describe_resources(resources) : cc::string(); !described.empty())
        cc::println("{}", described);
    return 0;
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

    // Get the static test registry
    auto& registry = get_static_test_registry();

    // Run NX_TEST_SETUP callbacks: they define aliases (with full registry access) and must run before any
    // listing or scheduling, so aliases are visible even when we only list/discover tests and never run them.
    nx::run_setup_callbacks(registry);

    // A binary whose defaults are ambiguous is broken whatever it was asked, so a test run fails on it too — which is
    // the run CI makes, where a bare run that only a user would notice is not.
    if (auto const problems = impl::check_default_entries(registry); !problems.empty())
    {
        cc::eprint("{}", problems);
        return 1;
    }

    // Name first: an app or command named by the first token, a nexus selector, a test named exactly, the default, or
    // the overview.
    auto tokens = cc::vector<cc::string_view>();
    for (auto i = 1; i < argc; ++i)
        tokens.push_back(cc::string_view(argv[i]));
    auto const route = impl::route_command_line(registry, tokens);

    if (route.kind == impl::entry_route_kind::overview)
    {
        cc::print("{}", impl::render_overview(registry, suite_name()));
        return 0;
    }
    if (route.kind == impl::entry_route_kind::error)
    {
        cc::eprintln("{}\n", route.message);
        cc::eprint("{}", impl::render_overview(registry, suite_name()));
        return 1;
    }

    auto const is_entry_run = route.kind == impl::entry_route_kind::entry;

    // An app or command gets the defaults of a real run and its own command line; nexus parses none of that line.
    auto config = is_entry_run ? test_schedule_config::create_from_args(1, argv)
                               : test_schedule_config::create_from_args(argc, argv);
    if (is_entry_run)
    {
        config.selected_bucket = route.entry->test_config.bucket;
        config.allow_cross_bucket_naming = false;
        config.filters = {route.entry->name};
        config.test_args = route.entry_args;
    }

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

    // The entry's name is a substring filter like any other, so a sibling whose name contains it is dropped here.
    if (is_entry_run)
        schedule.instances.remove_all_where([&](test_instance const& i) { return i.declaration != route.entry; });

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

    // First, so it is there whatever the run does next, and a failure anywhere below can be reproduced from the log.
    // Not under the Catch2 XML reporter, whose stdout is the report, and not for an example, which is one program run
    // whose transcript is its documentation.
    if (config.shuffle && !config.report_catch2_xml_results && !is_entry_run
        && config.selected_bucket != nx::config::test_bucket::example)
        cc::println("nexus: run seed {} (reproduce with --seed {})", config.seed, config.seed);

    if (config.verbose)
    {
        schedule.print();
        cc::println();
    }

    // A benchmark number is a statement about a machine, so the machine goes first — and it is meant to be copied
    // along with the result rather than read once.
    // Keyed on what was SCHEDULED rather than on which bucket was swept: naming a benchmark exactly pulls it in
    // across the bucket rule, and that run wants the machine described just as much as a sweep does.
    auto has_benchmarks = false;
    for (auto const& instance : schedule.instances)
        if (instance.declaration != nullptr
            && instance.declaration->test_config.bucket == nx::config::test_bucket::benchmark)
            has_benchmarks = true;

    auto benchmark_load_before = nx::bench::load_sample{};
    auto benchmark_pinned = false;
    if (has_benchmarks)
    {
        auto const& sys = nx::bench::describe_system();
        cc::println("host  {} {}  |  {}  |  {} logical cores  |  build {}  CC_ASSERT={}{}", sys.os, sys.arch, sys.cpu,
                    sys.logical_cores, sys.build, sys.assertions_enabled ? "on" : "off",
                    sys.is_provisional ? "  (system info provisional)" : "");

        if (config.benchmark_pin)
        {
            benchmark_pinned = nx::bench::try_pin_to_core(0);
            cc::println("pin   requested, {}", benchmark_pinned ? "achieved on core 0" : "REFUSED by the platform");
        }

        // The counters backend prints its own one-time notice when the PMU is unreachable, naming the grant script
        // to run — so nothing is said here rather than saying it twice and worse.

        // The first reading is what the second is a delta against, so this one only primes the OS counters.
        benchmark_load_before = nx::bench::sample_load();
    }

    // Stand the recorder up for the WHOLE run, never per test.
    // Per-test attribution rides the ambient chain instead, so a test that records nothing costs nothing, and a test
    // asking what it recorded gets an answer without anyone parsing history back to the start of the process.
    if (!config.no_recording)
    {
        nx::impl::begin_run_recording();

        // Started here rather than lazily: events already drained are gone, and the point of this file is to carry
        // what happened BEFORE the interesting sample as much as the sample itself.
        nx::impl::begin_run_capture(config.benchmark_rec_file);

        // A dump the crash handler and the hang watchdog can both write.
        //
        // Installed rather than left to the application, because in a test run there IS no application to install
        // it: a fault or a blown deadline is exactly when someone wants to know what every thread was doing, and
        // that is the one moment nothing will get a chance to set it up.
        // The arena is reserved now for the same reason the handler cannot allocate later.
        cc::rec::install_crash_dump({.path = nx::impl::run_dump_path()});
    }

    // Its baseline is taken here, so the load it reports afterwards covers the tests and nothing that set them up.
    auto reporting = run_reporting{.registry = &registry,
                                   .is_entry_run = is_entry_run,
                                   .has_benchmarks = has_benchmarks,
                                   .benchmark_load_before = benchmark_load_before,
                                   .benchmark_pinned = benchmark_pinned,
                                   .cpu_sampler = cc::make_unique<cc::process_cpu_sampler>()};

    // Armed for the whole run, including the hosted path below, which stops it from its own finish callback.
    //
    // After the schedule is built rather than before: a deadline that covers discovery would fire on a binary that
    // is slow to enumerate rather than on one that is stuck, and those are different problems.
    impl::start_hang_watchdog({.per_test_secs = config.test_timeout_secs, .per_run_secs = config.run_timeout_secs});

    // A host that owns the thread gets the run in steps, and the report once the last one finishes.
    // Its callbacks — a WebGPU readback, a timer — run only between steps, so a blocking run there would never see them.
    if (impl::has_host_event_loop())
    {
        struct hosted_run
        {
            test_schedule schedule;
            test_schedule_config config;
            run_reporting reporting;
            cc::unique_ptr<impl::test_run> run;
        };
        auto* const hosted
            = new hosted_run{.schedule = cc::move(schedule), .config = config, .reporting = cc::move(reporting)};
        hosted->run = cc::make_unique<impl::test_run>(hosted->schedule, hosted->config);
        impl::run_in_host_loop([hosted] { return hosted->run->step(); },
                               [hosted]
                               {
                                   impl::stop_hang_watchdog();
                                   auto const code
                                       = report_run(hosted->config, hosted->reporting, hosted->run->take_result());
                                   delete hosted;
                                   return code;
                               });
        return 0; // not reached: the host loop ends the process
    }

    auto const result = execute_tests(schedule, config);
    impl::stop_hang_watchdog();
    return report_run(config, reporting, result);
}
