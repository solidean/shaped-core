#include "run.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/catch2.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/listing_json.hh>
#include <nexus/tests/export/perf_json.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <fstream>     // std::ofstream: JUnit report file output
#include <iostream>    // std::cout / std::cerr: console output
#include <string_view> // std::string_view: streams a cc::string into std::ostream (no operator<<)

namespace
{
// cc::string is not std::ostream-streamable, so view it as a std::string_view.
std::string_view as_sv(cc::string_view s)
{
    return std::string_view(s.data(), size_t(s.size()));
}

void print_help()
{
    std::cout << "nexus - Unified test, fuzz, benchmark, and app runner for modern C++\n\n";
    // Advertise Catch2 compatibility to enable C++ TestMate IDE extension recognition
    std::cout << "Compatible with Catch2 v3.11.0 in some args\n\n";
    std::cout << "Usage:\n";
    std::cout << "  <test-executable> [options]\n\n";
    std::cout << "For more information, see the nexus documentation.\n";
}

// Derives a JUnit suite name from argv[0]: the basename without a directory or a
// trailing .exe extension (e.g. "build/bin/nexus-test.exe" -> "nexus-test").
cc::string program_name(char const* argv0)
{
    cc::string_view name = argv0 != nullptr ? argv0 : "nexus";

    // strip the directory (cc::string_view has no find_last_of, so rfind each separator)
    if (auto const slash = cc::max(name.rfind('/'), name.rfind('\\')); slash >= 0)
        name = name.subview(slash + 1);

    if (name.size() > 4)
    {
        auto const ext = name.subview(name.size() - 4);
        if (ext == ".exe" || ext == ".EXE")
            name = name.subview({.offset = 0, .size = name.size() - 4});
    }

    return cc::string(name);
}

// Writes a cc::string to an std::ostream by bytes (cc::string has no operator<<).
void write_to(std::ostream& os, cc::string_view s)
{
    os.write(s.data(), s.size());
}
} // namespace

int nx::run(int argc, char** argv)
{
    // Handle --help flag
    if (argc == 2 && cc::string_view(argv[1]) == "--help")
    {
        print_help();
        return 0;
    }

    // Create schedule config from command line arguments
    auto config = test_schedule_config::create_from_args(argc, argv);

    // Get the static test registry
    auto& registry = get_static_test_registry();

    // Handle Catch2 XML discovery mode for TestMate integration
    if (config.is_catch2_xml_discovery)
    {
        write_to(std::cout, write_catch2_discovery_xml(registry));
        return 0;
    }

    // JSON test listing: a query used by `dev.py test` to pre-select which binaries actually contain a matching
    // test. It reports every registered test plus eligibility under the parsed args; it never runs anything and
    // always succeeds, even when nothing is eligible (the caller decides what an empty match means).
    if (!config.list_tests_json_file.empty())
    {
        auto const json = write_test_listing_json(program_name(argv[0]), config, registry);
        if (config.list_tests_json_file == "-")
            write_to(std::cout, json);
        else
        {
            std::ofstream out(config.list_tests_json_file.c_str_materialize());
            if (out)
                write_to(out, json);
            else
            {
                std::cerr << "Error: could not open test listing JSON file: " << as_sv(config.list_tests_json_file)
                          << "\n";
                return 1;
            }
        }
        return 0;
    }

    // Create schedule from config and registry
    auto schedule = test_schedule::create(config, registry);

    // Check if any tests were scheduled
    if (schedule.instances.empty())
    {
        // A guide-benchmark sweep over a binary that has none is not an error: `dev.py pgo` runs
        // --guide-benchmarks across every test binary, and most contain no guide benchmarks.
        if (config.selected_bucket == nx::config::test_bucket::guide_benchmark)
        {
            std::cout << "No guide benchmarks in this binary\n";
            return 0;
        }

        std::cerr << "Error: The current schedule did not select any tests\n";
        for (int i = 0; i < argc; ++i)
        {
            std::cerr << "  arg[" << i << "] = `" << argv[i] << "'\n";
        }
        return 1;
    }

    if (config.verbose)
    {
        schedule.print();
        std::cout << std::endl; // NOLINT
    }

    // Execute the scheduled tests
    auto execution = execute_tests(schedule, config);

    // Write a JUnit XML report if requested. This is additive: the normal
    // console output below still runs regardless of the reporting mode.
    if (!config.junit_xml_file.empty())
    {
        std::ofstream out(config.junit_xml_file.c_str_materialize());
        if (out)
            write_to(out, write_junit_xml(program_name(argv[0]), execution));
        else
            std::cerr << "Error: could not open JUnit XML file: " << as_sv(config.junit_xml_file) << "\n";
    }

    // Write a perf-metrics JSON sidecar if requested (the metrics recorded via nx::guide). Also additive.
    if (!config.perf_json_file.empty())
    {
        std::ofstream out(config.perf_json_file.c_str_materialize());
        if (out)
            write_to(out, write_perf_json(program_name(argv[0]), execution));
        else
            std::cerr << "Error: could not open perf JSON file: " << as_sv(config.perf_json_file) << "\n";
    }

    // Handle Catch2 XML results reporting for TestMate integration
    if (config.report_catch2_xml_results)
    {
        write_to(std::cout, write_catch2_results_xml(execution));
        return execution.count_failed_tests() > 0 ? 1 : 0;
    }

    // Print any metrics recorded via nx::guide (guide benchmarks). Console-only mirror of the perf JSON sidecar.
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
            std::cout << "\nRecorded metrics:\n";
            for (auto const& exec : execution.executions)
                for (auto const& metric : exec.metrics)
                {
                    char const* const dir = metric.higher_is_better ? "(higher is better)" : "(lower is better)";
                    std::cout << "  " << as_sv(exec.instance.declaration->name) << " | " << as_sv(metric.name) << " = "
                              << metric.value << " " << as_sv(metric.unit) << " " << dir << "\n";
                }
        }
    }

    // Check for failures
    int const failed_tests = execution.count_failed_tests();
    int const total_tests = execution.count_total_tests();
    int const failed_checks = execution.count_failed_checks();
    int const total_checks = execution.count_total_checks();

    if (failed_tests > 0)
    {
        // Print failed test information
        std::cerr << "\nFailed tests:\n";
        for (auto const& exec : execution.executions)
        {
            if (exec.is_considered_failing())
            {
                auto const& decl = exec.instance.declaration;
                if (decl)
                {
                    std::cerr << "  " << as_sv(decl->name) << " at " << decl->location.file_name() << ":"
                              << decl->location.line() << "\n";
                }
            }
        }

        std::cerr << "\n" << failed_tests << " of " << total_tests << " tests failed\n";
        std::cerr << "Failed " << failed_checks << " of " << total_checks << " checks\n";
        return 1;
    }

    // All tests passed
    std::cout << "All " << total_tests << " tests passed (" << total_checks << " checks)\n";
    return 0;
}
