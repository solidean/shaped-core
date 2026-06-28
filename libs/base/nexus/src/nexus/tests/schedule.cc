#include "schedule.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/string_view.hh>

#include <iostream>    // std::cout: console output
#include <string_view> // std::string_view: streams a cc::string into std::ostream

namespace
{
// cc::string is not std::ostream-streamable, so view it as a std::string_view.
std::string_view as_sv(cc::string_view s)
{
    return std::string_view(s.data(), size_t(s.size()));
}
} // namespace

nx::test_schedule_config nx::test_schedule_config::create_from_args(int argc, char** argv)
{
    test_schedule_config config;

    // Track Catch2 compatibility flags for XML discovery mode
    bool has_verbosity = false;
    bool has_list_tests = false;
    bool has_xml_reporter = false;
    bool has_durations = false;

    // Set by --manual / --guide-benchmarks. When a bucket is chosen explicitly, a non-wildcard filter narrows
    // within that bucket rather than crossing into other buckets.
    bool explicit_bucket = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i)
    {
        cc::string_view const arg = argv[i];

        // Check for simple verbose flag
        if (arg == "-v")
        {
            config.verbose = true;
            continue;
        }
        // Manual mode: select the manual bucket, so wildcard filters can select among manual tests (e.g.
        // `--manual bench` runs every manual test whose name contains "bench"). Disabled tests stay out.
        else if (arg == "--manual")
        {
            config.selected_bucket = config::test_bucket::manual;
            explicit_bucket = true;
            continue;
        }
        // Guide-benchmark mode: select the guide_benchmark bucket (the tests that report metrics via nx::guide).
        else if (arg == "--guide-benchmarks")
        {
            config.selected_bucket = config::test_bucket::guide_benchmark;
            explicit_bucket = true;
            continue;
        }
        // Check for section filter flag
        else if (arg == "-c")
        {
            // Get the next argument as section name
            if (i + 1 < argc)
            {
                ++i;
                config.section_filters.emplace_back(argv[i]);
            }
            continue;
        }
        // Check for Catch2 compatibility flags (don't add to filters)
        else if (arg == "--verbosity")
        {
            has_verbosity = true;
            // Skip the next argument (verbosity level)
            if (i + 1 < argc)
                ++i;
            continue;
        }
        else if (arg == "--list-tests")
        {
            has_list_tests = true;
            continue;
        }
        else if (arg == "--reporter")
        {
            has_xml_reporter = true;
            // Skip the next argument (reporter type)
            if (i + 1 < argc)
                ++i;
            continue;
        }
        else if (arg == "--durations")
        {
            has_durations = true;
            // Skip the next argument (durations value)
            if (i + 1 < argc)
                ++i;
            continue;
        }
        // JUnit XML report file (consumed here so the path is not misread as a filter)
        else if (arg == "--junit-xml")
        {
            if (i + 1 < argc)
                config.junit_xml_file = argv[++i];
            continue;
        }
        // Perf-metrics JSON sidecar (consumed here so the path is not misread as a filter)
        else if (arg == "--perf-json")
        {
            if (i + 1 < argc)
                config.perf_json_file = argv[++i];
            continue;
        }
        // JSON test listing (consumed here so the path is not misread as a filter). The rest of the args still
        // parse normally, so the listing reflects exactly the filters/bucket a real run would use.
        else if (arg == "--list-tests-json")
        {
            if (i + 1 < argc)
                config.list_tests_json_file = argv[++i];
            continue;
        }

        // Regular filter argument - split by comma for Catch2 compatibility
        cc::isize start = 0;
        for (cc::isize end = arg.find(','); end >= 0; end = arg.find(',', start))
        {
            if (auto const filter = arg.subview({.start = start, .end = end}); !filter.empty())
                config.filters.emplace_back(filter);
            start = end + 1;
        }
        if (auto const filter = arg.subview(start); !filter.empty())
            config.filters.emplace_back(filter);
    }

    // Enable Catch2 XML discovery mode if all three flags are present
    config.is_catch2_xml_discovery = has_list_tests && has_xml_reporter;

    // Enable Catch2 XML results reporting if durations + xml reporter (and not list tests)
    config.report_catch2_xml_results = has_xml_reporter && !has_list_tests;

    // Normalize filters for Catch2 compatibility (postprocess)
    if (config.is_catch2_xml_discovery || config.report_catch2_xml_results)
    {
        for (auto& filter : config.filters)
        {
            // Catch2 escapes square brackets as \[; undo that.
            filter.replace_all("\\[", "[");
        }
    }

    // A non-wildcard filter names a single test explicitly, which is enough to pull in an otherwise
    // non-automatic test — disabled ones, and (unless a bucket was selected explicitly) tests of any bucket.
    if (!config.filters.empty())
    {
        for (auto const& filter : config.filters)
        {
            if (!filter.contains('*'))
            {
                config.run_disabled_tests = true;
                if (!explicit_bucket)
                    config.match_any_bucket = true;
                break;
            }
        }
    }

    return config;
}

bool nx::test_schedule_config::name_matches(test_declaration const& decl) const
{
    if (filters.empty())
        return true;

    for (auto const& filter : filters)
    {
        if (filter.empty())
            continue;

        // Simple substring match for now
        if (decl.name.contains(filter))
            return true;
    }

    return false;
}

bool nx::test_schedule_config::would_run(test_declaration const& decl) const
{
    auto const& tc = decl.test_config;

    // Eligibility by bucket + disabled status (filters are applied afterwards):
    //  - a sweep selects exactly one bucket (selected_bucket); a test in another bucket is excluded unless
    //    match_any_bucket is set (a non-wildcard filter without an explicit bucket flag names a test directly).
    //  - disabled is orthogonal: a disabled test runs only when explicitly requested (run_disabled_tests).
    if (!match_any_bucket && tc.bucket != selected_bucket)
        return false;

    if (!tc.enabled && !run_disabled_tests)
        return false;

    return name_matches(decl);
}

nx::test_schedule nx::test_schedule::create(test_schedule_config const& config, test_registry const& registry)
{
    test_schedule schedule;

    for (auto const& decl : registry.declarations)
    {
        CC_ASSERT(decl.function.is_valid(), "invalid test decl");

        if (!config.would_run(decl))
            continue;

        schedule.instances.push_back(test_instance{
            .declaration = &decl,
        });
    }

    return schedule;
}

void nx::test_schedule::print() const
{
    std::cout << "test schedule:\n";
    for (auto const& instance : instances)
    {
        std::cout << "  - \"" << as_sv(instance.declaration->name) << "\"\n";
    }
}
