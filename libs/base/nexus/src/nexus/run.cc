#include "run.hh"

#include <nexus/tests/execute.hh>
#include <nexus/tests/export/catch2.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
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
std::string program_name(char const* argv0)
{
    std::string_view name = argv0 != nullptr ? argv0 : "nexus";

    if (auto const slash = name.find_last_of("/\\"); slash != std::string_view::npos)
        name = name.substr(slash + 1);

    if (name.size() > 4)
    {
        auto const ext = name.substr(name.size() - 4);
        if (ext == ".exe" || ext == ".EXE")
            name = name.substr(0, name.size() - 4);
    }

    return std::string(name);
}
} // namespace

int nx::run(int argc, char** argv)
{
    // Handle --help flag
    if (argc == 2 && std::string_view(argv[1]) == "--help")
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
        write_catch2_discovery_xml(std::cout, registry);
        return 0;
    }

    // Create schedule from config and registry
    auto schedule = test_schedule::create(config, registry);

    // Check if any tests were scheduled
    if (schedule.instances.empty())
    {
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
        std::ofstream out(config.junit_xml_file);
        if (out)
            write_junit_xml(out, program_name(argv[0]), execution);
        else
            std::cerr << "Error: could not open JUnit XML file: " << config.junit_xml_file << "\n";
    }

    // Handle Catch2 XML results reporting for TestMate integration
    if (config.report_catch2_xml_results)
    {
        write_catch2_results_xml(std::cout, execution);
        return execution.count_failed_tests() > 0 ? 1 : 0;
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
                    std::cerr << "  " << decl->name << " at " << decl->location.file_name() << ":"
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
