#include "schedule.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/string/glob.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh> // also what puts the bare sized aliases in scope inside nx

#include <iostream>    // std::cout: console output
#include <string_view> // std::string_view: streams a cc::string into std::ostream

using namespace cc::primitive_defines;

namespace
{
// cc::string is not std::ostream-streamable, so view it as a std::string_view.
std::string_view as_sv(cc::string_view s)
{
    return std::string_view(s.data(), size_t(s.size()));
}

// Does one glob select `path`? Both sides are already normalized, and matching folds case — a path is a path,
// and the recorded file is spelled however the compiler was invoked.
//
// A pattern is also tried as a path *suffix*, which is what lets a bare filename or a repo-relative fragment
// select against the absolute path a test declaration carries.
// A pattern naming no wildcard tail additionally stands for its subtree, so a directory selects everything under it.
bool path_matches_glob(cc::string_view pattern, cc::string_view path)
{
    // Both sides are normalized by their callers — once per filter and once per test, rather than once per pairing.
    constexpr auto k_options = cc::flags(cc::glob_option::ignore_case);

    auto const p = cc::glob_normalize_path(pattern);
    if (p.empty())
        return false;

    auto const anchored = cc::string("**/") + p;
    if (cc::glob_matches(p, path, k_options) || cc::glob_matches(anchored, path, k_options))
        return true;

    if (!cc::string_view(p).ends_with('*'))
        return cc::glob_matches(p + "/**", path, k_options) || cc::glob_matches(anchored + "/**", path, k_options);

    return false;
}

// True if any non-empty filter selects `file`, the source path a declaration recorded.
bool any_file_matches(cc::span<cc::string const> filters, char const* file)
{
    if (file == nullptr)
        return false;

    auto const path = cc::glob_normalize_path(file);
    for (auto const& filter : filters)
        if (!filter.empty() && path_matches_glob(filter, path))
            return true;

    return false;
}

// True if any non-empty filter is a substring of `name`.
bool any_name_matches(cc::span<cc::string const> filters, cc::string_view name)
{
    for (auto const& filter : filters)
        if (!filter.empty() && name.contains(filter))
            return true;

    return false;
}

// Element-wise equality of two section paths (cc::vector has no operator==).
bool same_path(cc::span<cc::string const> a, cc::span<cc::string const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
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

    // Set by --manual / --guide-benchmarks.
    // With a bucket chosen explicitly, an exact filter narrows within that bucket rather than crossing into another.
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
        // Read the filters as file globs over the tests' source files, skipping name matching entirely.
        else if (arg == "--match-files")
        {
            config.mode = filter_mode::file;
            continue;
        }
        // Read the filters as test names only, without the file-glob fallback.
        else if (arg == "--match-names")
        {
            config.mode = filter_mode::name;
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
        isize start = 0;
        for (isize end = arg.find(','); end >= 0; end = arg.find(',', start))
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

    // Without an explicit bucket flag, a filter may reach a test in another bucket; is_eligible decides that per test, on an exact name.
    config.allow_cross_bucket_naming = !explicit_bucket;

    return config;
}

bool nx::test_schedule_config::filter_matches(test_declaration const& decl) const
{
    if (filters.empty())
        return true;

    if (matching_files)
        return any_file_matches(filters, decl.location.file_name());

    // Simple substring match for now
    return any_name_matches(filters, decl.name);
}

bool nx::test_schedule_config::name_matches_exact(test_declaration const& decl) const
{
    for (auto const& filter : filters)
        if (!filter.empty() && cc::string_view(decl.name) == cc::string_view(filter))
            return true;
    return false;
}

bool nx::test_schedule_config::is_eligible(test_declaration const& decl, bool named_exactly) const
{
    auto const& tc = decl.test_config;

    // Bucket and disabled are two independent gates, both keyed on the *exact* name; a substring filter opens neither.
    // `named_exactly` is that key: the test's own name for a direct match, the alias name for an alias fragment.
    // Filters themselves are applied by the caller.
    //  - a sweep selects exactly one bucket, and a test in another bucket runs only when named exactly AND no bucket flag was given.
    //    Under --manual the sweep is the manual bucket, period.
    //  - disabled is orthogonal: a disabled test runs when named exactly, or under bulk run_disabled_tests.
    if (tc.bucket != selected_bucket && !(allow_cross_bucket_naming && named_exactly))
        return false;

    if (!tc.enabled && !run_disabled_tests && !named_exactly)
        return false;

    return true;
}

bool nx::test_schedule_config::would_run(test_declaration const& decl) const
{
    return is_eligible(decl, name_matches_exact(decl)) && filter_matches(decl);
}

bool nx::test_schedule_config::alias_filter_matches(test_alias const& alias) const
{
    // Empty filters = a full sweep, which already runs every driver unscoped; do not expand aliases then.
    if (filters.empty())
        return false;

    if (matching_files)
        return any_file_matches(filters, alias.location.file_name());

    return any_name_matches(filters, alias.name);
}

bool nx::test_schedule_config::alias_matches_exact(test_alias const& alias) const
{
    for (auto const& filter : filters)
        if (!filter.empty() && cc::string_view(alias.name) == cc::string_view(filter))
            return true;
    return false;
}

void nx::test_schedule_config::resolve_filter_mode(test_registry const& registry)
{
    if (mode != filter_mode::name_or_file)
    {
        matching_files = mode == filter_mode::file;
        return;
    }

    // A full sweep has nothing to fall back from.
    if (filters.empty())
        return;

    for (auto const& decl : registry.declarations)
        if (any_name_matches(filters, decl.name))
            return;

    for (auto const& alias : registry.aliases)
        if (any_name_matches(filters, alias.name))
            return;

    matching_files = true;
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

    // Aliases act purely as filters: a matched alias name selects (driver, section-path) leaves to run.
    // Every matched fragment sharing a driver is grouped into that driver's ONE instance, whose scope set is the union of their paths.
    // The bucket and disabled gates still apply, keyed on the *alias* name, so reaching a manual driver through one of its aliases takes that alias's exact name.
    //
    // A fragment is dropped when its target run is already covered, so nothing executes twice:
    //  - the driver is already scheduled *unscoped* (matched directly by name) — that run drives every
    //    invocable, including this fragment's, so any scoped instance is redundant;
    //  - its exact (driver, section path) is already in the driver's scope set (two aliases sharing a fragment).
    for (auto const& alias : registry.aliases)
    {
        if (!config.alias_filter_matches(alias))
            continue;

        bool const named_exactly = config.alias_matches_exact(alias);

        for (auto const& frag : alias.fragments)
        {
            if (frag.driver == nullptr)
                continue;

            // Each fragment carries its own driver, so the gate is per-driver; only the exact-name key is
            // shared across the alias.
            if (!config.is_eligible(*frag.driver, named_exactly))
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
