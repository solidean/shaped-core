#include "schedule.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/glob.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/args/args.hh>
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

// Decimal, no sign, no whitespace; -1 for anything else, including an empty view.
int parse_count(cc::string_view s)
{
    if (s.empty())
        return -1;

    int value = 0;
    for (auto const c : s)
    {
        if (c < '0' || c > '9')
            return -1;
        value = value * 10 + (c - '0');
        if (value > 4096) // far past any plausible job count, and it keeps the accumulate from overflowing
            return -1;
    }
    return value;
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

namespace
{
// The Catch2 compat flags are about which arguments were SEEN, not what they were set to: the values of
// --reporter, --verbosity and --durations are consumed and deliberately ignored.
struct cli_state
{
    bool has_list_tests = false;
    bool has_xml_reporter = false;
    bool explicit_bucket = false;

    cc::vector<cc::string> positionals;
    cc::vector<cc::string_view> unknown;
    cc::vector<cc::string_view> passthrough;
    cc::string test_args_line;
};

// The whole CLI, in one place.
// Shared by the parse and by --help, so the two cannot describe different programs — which is exactly what
// the hand-written help block this replaced had drifted into.
nx::args_builder build_cli(nx::test_schedule_config& config, cli_state& state)
{
    using nx::filter_mode;
    namespace config_ns = nx::config;

    auto args = nx::args({
        .name = "nexus",
        .description = "Unified test, fuzz, benchmark, and app runner for modern C++",

        // Load-bearing: C++ TestMate identifies a Catch2-compatible binary by running --help and scanning
        // its output for this version string.
        // Changing or dropping it silently stops an IDE from discovering any test in this binary.
        // libs/base/nexus/docs/catch2-runner-compat.md is the contract.
        .help = "Compatible with Catch2 v3.11.0 in some args",
    });

    // Anything unrecognized is a filter, which is what nexus has always done, and what lets a test whose
    // name begins with a dash be selected at all.
    args.allow_unknown(state.unknown);
    args.no_auto_completion();

    args.positional("FILTER", state.positionals,
                    {.desc = "run only tests whose name contains this, or whose source file matches it"});

    // Documented rather than left to the built-ins, because nx::run intercepts both before this parser ever
    // sees them — it is the harness that decides to print and exit.
    args.action({"h"}, [] {}, "show this help and exit");
    args.action({"help"}, [] {}, "show this help and exit");

    args.arg({"v"}, config.verbose, "print the schedule before running it");
    args.arg({"c"}, config.section_filters, {.desc = "run only sections matching this name", .metavar = "NAME"});

    // Not bound directly: a bad count has always warned and kept the default rather than failing the run,
    // and a migration is the wrong place to start rejecting command lines that used to work.
    args.value_action({"j", "jobs"},
                      [&config](cc::string_view value)
                      {
                          if (auto const count = parse_count(value); count >= 0)
                              config.jobs = count;
                          else
                              cc::eprintln("nexus: ignoring `{}`: expected a job count", value);
                      },
                      {.desc = "how many tests may run at once; 0 means one per hardware thread", .metavar = "N"});

    args.group("selection");
    args.action(
        {"manual"},
        [&]
        {
            config.selected_bucket = config_ns::test_bucket::manual;
            state.explicit_bucket = true;
        },
        "sweep the manual bucket instead of the normal one");
    args.action(
        {"guide-benchmarks"},
        [&]
        {
            config.selected_bucket = config_ns::test_bucket::guide_benchmark;
            state.explicit_bucket = true;
        },
        "sweep the guide-benchmark bucket");
    args.action(
        {"examples"},
        [&]
        {
            config.selected_bucket = config_ns::test_bucket::example;
            state.explicit_bucket = true;
        },
        "sweep the example bucket");
    args.action(
        {"match-files"}, [&config] { config.mode = filter_mode::file; }, "read the filters as globs over source files");
    args.action({"match-names"}, [&config] { config.mode = filter_mode::name; }, "read the filters as test names only");

    args.group("recording");
    args.arg({"record"}, config.record_all, "bucket every test's cc::rec events, whatever its own config says");
    args.arg({"no-recording"}, config.no_recording, "leave cc::rec down for the whole run");

    args.group("reports");
    args.arg({"junit-xml"}, config.junit_xml_file, {.desc = "also write a JUnit XML report here", .metavar = "FILE"});
    args.arg({"perf-json"}, config.perf_json_file, {.desc = "also write nx::guide metrics here", .metavar = "FILE"});
    args.arg({"list-tests-json"}, config.list_tests_json_file,
             {.desc = "write a JSON listing of every test here (- for stdout) and exit", .metavar = "FILE"});

    args.group("arguments for the test itself");
    args.arg({"test-args"}, state.test_args_line,
             {.desc = "a command line for the selected test, reachable from it through nx::current_args()",
              .metavar = "LINE"});
    args.rest(state.passthrough, "ARGS", "the same, for everything after a bare --");

    args.group("Catch2 compatibility");
    args.action(
        {"list-tests"}, [&state] { state.has_list_tests = true; },
        "with --reporter, emit the XML listing an IDE discovers through");
    args.value_action({"reporter"}, [&state](cc::string_view) { state.has_xml_reporter = true; },
                      {.desc = "which reporter to use; the value is accepted and ignored", .metavar = "TYPE"});
    args.value_action({"verbosity"}, [](cc::string_view) {}, {.desc = "accepted and ignored", .metavar = "LEVEL"});
    args.value_action({"durations"}, [](cc::string_view) {}, {.desc = "accepted and ignored", .metavar = "YES/NO"});

    // The harness owns printing and exiting, so the parse only reports.
    args.no_auto_print();
    args.no_auto_help();
    return args;
}
} // namespace

nx::test_schedule_config nx::test_schedule_config::create_from_args(int argc, char** argv)
{
    auto config = test_schedule_config();

    // A real run uses every core unless --jobs says otherwise, which is the opposite of the struct's own default.
    // A suite that only passes one test at a time is hiding something, and the place to find that out is the ordinary run.
    // A hand-built config keeps schedule order, because a test that builds a schedule is usually asserting about it.
    config.jobs = 0;

    auto state = cli_state();
    auto args = build_cli(config, state);
    args.parse(argc, argv);

    // Unrecognized tokens rejoin the filters, which is where they always went.
    for (auto const& token : state.unknown)
        state.positionals.push_back(cc::string(token));

    // One filter argument may carry several, comma-separated, which is the Catch2 convention.
    // Empty pieces are dropped: a filter that matches everything is never what a stray comma meant.
    for (auto const& value : state.positionals)
    {
        auto const arg = cc::string_view(value);
        auto start = isize(0);
        for (auto end = arg.find(','); end >= 0; end = arg.find(',', start))
        {
            if (auto const filter = arg.subview({.start = start, .end = end}); !filter.empty())
                config.filters.emplace_back(filter);

            start = end + 1;
        }

        if (auto const filter = arg.subview(start); !filter.empty())
            config.filters.emplace_back(filter);
    }

    // Everything after -- arrives already split; --test-args carries one string that still needs tokenizing.
    for (auto const& token : state.passthrough)
        config.test_args.push_back(cc::string(token));

    for (auto& token : nx::args_tokenize(state.test_args_line))
        config.test_args.push_back(cc::move(token));

    config.is_catch2_xml_discovery = state.has_list_tests && state.has_xml_reporter;
    config.report_catch2_xml_results = state.has_xml_reporter && !state.has_list_tests;

    if (config.is_catch2_xml_discovery || config.report_catch2_xml_results)
    {
        for (auto& filter : config.filters)
        {
            // Catch2 escapes an opening square bracket as \[, because it opens tag syntax; undo that.
            // The closing one is left as typed, which is what nexus has always done.
            filter.replace_all("\\[", "[");
        }
    }

    // Without an explicit bucket flag, a filter may reach a test in another bucket; is_eligible decides that per test, on an exact name.
    config.allow_cross_bucket_naming = !state.explicit_bucket;

    return config;
}

cc::string nx::test_schedule_config::cli_help_text()
{
    // A throwaway config, because help describes the declaration rather than any particular parse.
    auto config = test_schedule_config();
    auto state = cli_state();
    auto args = build_cli(config, state);

    return args.help_text({.color = cc::console::color_enabled(), .width = cc::console::terminal_width().value_or(100)});
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

        CC_ASSERT(decl.function.is_valid() || decl.is_async(), "invalid test decl");

        // Checked here rather than left to the order the config objects happen to be spelled in.
        // `owns_recorder` tears the recorder down for every thread at once, so a test holding it while another runs is
        // not a rule about this test — it breaks the other one.
        // Untagged exclusive specifically: a TAGGED one only excludes fellow tag holders, which leaves every other
        // test in the run free to be recording into a recorder this one is about to tear down.
        CC_ASSERT(!decl.test_config.owns_recorder || decl.test_config.exclusive_global,
                  "nx::config::owns_recorder requires an untagged nx::config::exclusive() — the recorder is "
                  "process-wide, so handing it to one test takes it away from every test running alongside it");
        CC_ASSERT(!decl.test_config.owns_recorder || !decl.test_config.recorded,
                  "nx::config::owns_recorder and nx::config::recorded are mutually exclusive — a test that owns the "
                  "recorder has no run recorder to be bucketed into");

        if (!config.would_run(decl))
            continue;

        schedule.instances.push_back(test_instance{
            .declaration = &decl,
            .registry = &registry,
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
                schedule.instances.push_back(test_instance{.declaration = frag.driver, .registry = &registry});
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
