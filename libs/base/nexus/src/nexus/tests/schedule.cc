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

// Element-wise equality of two section paths (cc::vector has no operator==).
bool same_path(cc::span<cc::string const> a, cc::span<cc::string const> b)
{
    if (a.size() != b.size())
        return false;
    for (cc::isize i = 0; i < a.size(); ++i)
        if (cc::string_view(a[i]) != cc::string_view(b[i]))
            return false;
    return true;
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

bool nx::test_schedule_config::alias_matches(test_alias const& alias) const
{
    // Empty filters = a full sweep, which already runs every driver unscoped; do not expand aliases then.
    if (filters.empty())
        return false;

    for (auto const& filter : filters)
    {
        if (filter.empty())
            continue;

        if (alias.name.contains(filter))
            return true;
    }

    return false;
}

nx::test_schedule nx::test_schedule::create(test_schedule_config const& config, test_registry const& registry)
{
    test_schedule schedule;
    schedule.registry = &registry;

    for (auto const& decl : registry.declarations)
    {
        // Parametrized tests are inert: a sweep never schedules them; they run only via nx::invoke_tests.
        if (decl.is_invocable())
            continue;

        CC_ASSERT(decl.function.is_valid(), "invalid test decl");

        if (!config.would_run(decl))
            continue;

        schedule.instances.push_back(test_instance{
            .declaration = &decl,
        });
    }

    // Aliases act purely as filters: a matched alias name selects (driver, section-path) leaves to run. Every
    // matched fragment that shares a driver is grouped into that driver's ONE instance, whose scope set is the
    // union of their paths — so a driver's body runs exactly once regardless of how many of its aliases match
    // (aliases never add schedule entries in an additive sense). Bucket/disabled gates are bypassed for these,
    // like a directly named test.
    //
    // A fragment is dropped when its target run is already covered, so nothing executes twice:
    //  - the driver is already scheduled *unscoped* (matched directly by name) — that run drives every
    //    invocable, including this fragment's, so any scoped instance is redundant;
    //  - its exact (driver, section path) is already in the driver's scope set (two aliases sharing a fragment).
    for (auto const& alias : registry.aliases)
    {
        if (!config.alias_matches(alias))
            continue;

        for (auto const& frag : alias.fragments)
        {
            if (frag.driver == nullptr)
                continue;

            // Covered by an unscoped run of this driver? then everything under it runs already.
            bool covered_unscoped = false;
            for (auto const& inst : schedule.instances)
                if (inst.declaration == frag.driver && inst.section_scopes.empty())
                {
                    covered_unscoped = true;
                    break;
                }
            if (covered_unscoped)
                continue;

            // Find (or create) the single scoped instance for this driver, then add this path (deduped).
            test_instance* scoped = nullptr;
            for (auto& inst : schedule.instances)
                if (inst.declaration == frag.driver && !inst.section_scopes.empty())
                {
                    scoped = &inst;
                    break;
                }
            if (scoped == nullptr)
            {
                schedule.instances.push_back(test_instance{.declaration = frag.driver});
                scoped = &schedule.instances.back();
            }

            bool already = false;
            for (auto const& path : scoped->section_scopes)
                if (same_path(path, frag.section_path))
                {
                    already = true;
                    break;
                }
            if (!already)
                scoped->section_scopes.push_back(frag.section_path);
        }
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
