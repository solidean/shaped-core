#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/args/impl/describe.hh> // the dump is internal, but it is what completion is built on
#include <nexus/test.hh>

// Completion, and the description dump it is generated from.
//
// The dump is asserted structurally and each shell only spot-checked: a full golden per shell would pin
// four script dialects to the character, and every one of them would churn on an unrelated change.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_tool(int& jobs, bool& verbose, cc::string& mode)
{
    auto args = nx::args({.name = "mytool", .description = "does the thing"});
    args.no_auto_print();
    args.arg({"j", "jobs"}, jobs, "how many jobs to run at once");
    args.arg({"v", "verbose"}, verbose, "print more");
    args.arg({"m", "mode"}, mode, {.desc = "how to run", .validate = nx::arg::one_of({"fast", "slow"})});
    args.arg({"s", "secret"}, verbose, {.desc = "internal", .hidden = true});
    return args;
}
} // namespace

TEST("args completion - the description dump covers options and commands")
{
    auto jobs = 4;
    auto verbose = false;
    auto mode = cc::string();
    auto args = make_tool(jobs, verbose, mode);
    args.command({"build"}, "build the project", [](nx::args_builder& sub) { sub.info({.name = "build"}); });

    auto const described = nx::impl::describer::run(args);

    CHECK(described.name == "mytool");
    REQUIRE(described.commands.size() == 1);
    CHECK(described.commands[0].name == "build");
    CHECK(described.commands[0].desc == "build the project");

    SECTION("a hidden option is absent, as it is from help")
    {
        for (auto const& option : described.options)
            for (auto const& spelling : option.spellings)
                CHECK(spelling != "--secret");
    }

    SECTION("value-taking is recorded, because a shell has to know whether to expect one")
    {
        auto found_jobs = false;
        for (auto const& option : described.options)
            for (auto const& spelling : option.spellings)
                if (spelling == "--jobs")
                {
                    found_jobs = true;
                    CHECK(option.takes_value);
                    CHECK(option.metavar == "INT");
                }

        CHECK(found_jobs);
    }
}

TEST("args completion - describing forces every subtree")
{
    auto declared = 0;
    auto args = nx::args({.name = "mytool"});
    args.no_auto_print();
    args.command({"build"}, "build it", [&](nx::args_builder&) { ++declared; });
    args.command({"deploy"}, "ship it", [&](nx::args_builder&) { ++declared; });

    CHECK(declared == 0);

    // A completion script has to know about commands this run will never touch.
    auto const described = nx::impl::describer::run(args);
    CHECK(declared == 2);
    CHECK(described.commands.size() == 2);
}

TEST("args completion - a delegate contributes only its name")
{
    auto args = nx::args({.name = "mytool"});
    args.no_auto_print();
    args.delegate({"external"}, "hand off", [](cc::span<cc::string_view const>) { return 0; });

    auto const described = nx::impl::describer::run(args);
    REQUIRE(described.commands.size() == 1);
    CHECK(described.commands[0].name == "external");
    CHECK(described.commands[0].opaque);

    // Guessing at options we cannot see would be worse than offering nothing.
    CHECK(described.commands[0].options.empty());
}

TEST("args completion - an inherited global is completable at depth")
{
    auto verbose = false;
    auto jobs = 1;
    auto args = nx::args({.name = "mytool"});
    args.no_auto_print();
    args.arg({"v", "verbose"}, verbose, "print more");
    args.global();
    args.command({"build"}, "build it", [&](nx::args_builder& sub) { sub.arg({"j", "jobs"}, jobs, "how many"); });

    auto const described = nx::impl::describer::run(args);
    REQUIRE(described.commands.size() == 1);

    auto found_global = false;
    for (auto const& option : described.commands[0].options)
        for (auto const& spelling : option.spellings)
            if (spelling == "--verbose")
                found_global = true;

    CHECK(found_global);
}

TEST("args completion - each shell emits something it would accept")
{
    auto jobs = 4;
    auto verbose = false;
    auto mode = cc::string();
    auto args = make_tool(jobs, verbose, mode);
    args.command({"build"}, "build the project", [](nx::args_builder&) {});

    SECTION("bash")
    {
        auto const script = nx::generate_completion(args, nx::completion_shell::bash);
        CHECK(script.contains("_mytool_complete()"));
        CHECK(script.contains("complete -F _mytool_complete mytool"));
        CHECK(script.contains("--jobs"));
        CHECK(script.contains("build"));
        CHECK(!script.contains("--secret"));
    }

    SECTION("zsh")
    {
        auto const script = nx::generate_completion(args, nx::completion_shell::zsh);
        CHECK(script.contains("#compdef mytool"));
        CHECK(script.contains("_arguments"));
        CHECK(script.contains("--jobs[how many jobs to run at once]"));
    }

    SECTION("fish")
    {
        auto const script = nx::generate_completion(args, nx::completion_shell::fish);
        CHECK(script.contains("complete -c mytool -l jobs -r"));
        CHECK(script.contains("__fish_use_subcommand -a build"));
    }

    SECTION("powershell")
    {
        auto const script = nx::generate_completion(args, nx::completion_shell::powershell);
        CHECK(script.contains("Register-ArgumentCompleter -Native -CommandName mytool"));
        CHECK(script.contains("CompletionResult"));
        CHECK(script.contains("--jobs"));
    }
}

TEST("args completion - --completion is answered by the parse")
{
    auto jobs = 4;
    auto verbose = false;
    auto mode = cc::string();
    auto args = make_tool(jobs, verbose, mode);

    SECTION("as its own outcome, exiting zero")
    {
        auto const r = args.parse({"--completion", "bash"});
        CHECK(r.outcome() == nx::args_outcome::completion_requested);
        CHECK(r.exit_code() == 0);
        CHECK(!r.has_diagnostics());
    }

    SECTION("the = form works too")
    {
        CHECK(args.parse({"--completion=fish"}).outcome() == nx::args_outcome::completion_requested);
    }

    SECTION("an unknown shell says which ones there are")
    {
        auto const r = args.parse({"--completion", "csh"});
        CHECK(!r.ok());
        REQUIRE(r.has_diagnostics());
        CHECK(r.diagnostics()[0].message.contains("bash, zsh, fish or powershell"));
    }

    SECTION("and it can be turned off")
    {
        auto other = make_tool(jobs, verbose, mode);
        other.no_auto_completion();

        auto const r = other.parse({"--completion", "bash"});
        CHECK(r.outcome() == nx::args_outcome::usage_error);
    }
}
