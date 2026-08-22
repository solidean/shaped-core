#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Diagnostics as DATA: kind, token, arg name and suggestion.
// The rendered wording is pinned once per kind, further down, so improving a sentence touches one place.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    return args;
}
} // namespace

TEST("args diagnostic - an unknown option names itself and suggests the near miss")
{
    auto force = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");

    auto const r = args.parse({"--forse"});
    CHECK(!r.ok());
    CHECK(r.outcome() == nx::args_outcome::usage_error);
    CHECK(r.exit_code() == 1);

    REQUIRE(r.diagnostics().size() == 1);
    auto const& d = r.diagnostics()[0];
    CHECK(d.kind == nx::diagnostic_kind::unknown_option);
    CHECK(d.token == "--forse");
    CHECK(d.suggestion == "--force");
    CHECK(d.source.index == 0);
}

TEST("args diagnostic - a long name typed with one dash says exactly that")
{
    auto force = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");

    // The point of this case: NOT "unknown options -o -r -c -e".
    auto const r = args.parse({"-force"});
    REQUIRE(r.diagnostics().size() == 1);

    auto const& d = r.diagnostics()[0];
    CHECK(d.kind == nx::diagnostic_kind::unknown_option);
    CHECK(d.message.contains("not a cluster of short options"));
    CHECK(d.suggestion == "--force");
}

TEST("args diagnostic - a hidden alias is still suggested")
{
    auto force = false;
    auto args = make_args();
    args.arg({"force", nx::arg::hidden("legacy-force")}, force, "do it");

    // Half-remembering a deprecated name is exactly when a suggestion helps most.
    auto const r = args.parse({"--legacy-forse"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].suggestion == "--legacy-force");
}

TEST("args diagnostic - a hidden alias parses but stays out of help")
{
    auto force = false;
    auto args = make_args();
    args.arg({"force", nx::arg::hidden("legacy-force")}, force, "do it");

    CHECK(args.parse({"--legacy-force"}).ok());
    CHECK(force);

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("--force"));
    CHECK(!help.contains("legacy-force"));
}

TEST("args diagnostic - a missing value")
{
    auto jobs = 0;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    auto const r = args.parse({"--jobs"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::missing_value);
    CHECK(r.diagnostics()[0].arg_name == "--jobs");
}

TEST("args diagnostic - a value where none is taken")
{
    auto count = 0;
    auto args = make_args();
    args.count({"v", "verbose"}, count, "louder");

    auto const r = args.parse({"--verbose=3"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unexpected_value);
}

TEST("args diagnostic - a missing required option")
{
    auto output = cc::string();
    auto args = make_args();
    args.arg({"o", "output"}, output, {.desc = "where to write", .required = true});

    auto const r = args.parse({});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::missing_required);
    CHECK(r.diagnostics()[0].arg_name == "--output");
}

TEST("args diagnostic - too many and too few positionals")
{
    SECTION("too many")
    {
        auto one = cc::string();
        auto args = make_args();
        args.positional("ONE", one, {.desc = "the only one"});

        auto const r = args.parse({"a", "b"});
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unexpected_positional);
        CHECK(r.diagnostics()[0].token == "b");
    }

    SECTION("below a variadic's minimum")
    {
        auto files = cc::vector<cc::string>();
        auto args = make_args();
        args.positional("FILES", files, {.desc = "inputs", .min_count = 2});

        auto const r = args.parse({"a"});
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::missing_positional);
    }

    SECTION("above a variadic's maximum")
    {
        auto files = cc::vector<cc::string>();
        auto args = make_args();
        args.positional("FILES", files, {.desc = "inputs", .max_count = 1});

        auto const r = args.parse({"a", "b"});
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unexpected_positional);
    }
}

TEST("args diagnostic - everything wrong comes back from one run")
{
    auto jobs = 0;
    auto output = cc::string();
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");
    args.arg({"o", "output"}, output, {.desc = "where", .required = true});

    // One run, three problems: fixing a command line should not take three attempts.
    auto const r = args.parse({"--mystery", "--jobs", "abc"});
    REQUIRE(r.diagnostics().size() == 3);

    // Token order first, then what only the end of the parse can know.
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unknown_option);
    CHECK(r.diagnostics()[1].kind == nx::diagnostic_kind::invalid_value);
    CHECK(r.diagnostics()[2].kind == nx::diagnostic_kind::missing_required);
}

TEST("args diagnostic - help and version are outcomes, not errors")
{
    auto args = nx::args({.name = "t", .description = "a thing", .version = "1.2"});
    args.no_auto_print();

    SECTION("--help")
    {
        auto const r = args.parse({"--help"});
        CHECK(!r.ok());
        CHECK(r.should_exit());
        CHECK(r.exit_code() == 0); // the classic bug is returning failure here
        CHECK(r.outcome() == nx::args_outcome::help_requested);
        CHECK(!r.has_diagnostics());
    }

    SECTION("--version")
    {
        auto const r = args.parse({"--version"});
        CHECK(r.exit_code() == 0);
        CHECK(r.outcome() == nx::args_outcome::version_requested);
    }

    SECTION("help wins over anything else on the line")
    {
        auto const r = args.parse({"--nonsense", "--help"});
        CHECK(r.outcome() == nx::args_outcome::help_requested);
        CHECK(!r.has_diagnostics());
    }
}

TEST("args diagnostic - a declared name takes precedence over the built-in one")
{
    auto help_flag = cc::string();
    auto args = nx::args({.name = "t", .version = "1.0"});
    args.no_auto_print();
    args.arg({"help"}, help_flag, "what to explain");

    auto const r = args.parse({"--help", "topics"});
    CHECK(r.ok());
    CHECK(help_flag == "topics");
}

TEST("args diagnostic - usage_exit_code is configurable")
{
    auto args = make_args();
    args.usage_exit_code(2);

    CHECK(args.parse({"--nope"}).exit_code() == 2);
}

// --- rendering: one golden per kind, so the wording lives in exactly one place ------------------------

TEST("args diagnostic - the rendered form of an unknown option")
{
    auto force = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");

    auto const r = args.parse({"--forse"});
    CHECK(r.diagnostic_text({.color = false}) == R"(error: unknown option '--forse'
  did you mean --force?)");
}

TEST("args diagnostic - rendering never reads the process colour flag")
{
    auto force = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");

    auto const r = args.parse({"--nope"});

    // The pure form takes the flag explicitly, which is what keeps a golden independent of how the test
    // binary happened to be invoked.
    CHECK(!r.diagnostic_text({.color = false}).contains("\033["));
    CHECK(r.diagnostic_text({.color = true}).contains("\033["));
}
