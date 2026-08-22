#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Declaration bugs, which are the program's fault rather than the user's.
//
// These must hold with CC_ASSERT off, which is why this suite is also run under a release preset: a
// duplicate short name in a shipped binary is exactly the case that must not quietly do the wrong thing.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    return args;
}

/// Every setup failure looks the same from outside: its own kind, its own exit code, and never a guess.
void check_is_setup_error(nx::args_result const& r)
{
    CHECK(r.should_exit());
    CHECK(r.outcome() == nx::args_outcome::setup_error);
    CHECK(r.exit_code() == nx::args_setup_exit_code);
    REQUIRE(r.has_diagnostics());

    for (auto const& d : r.diagnostics())
    {
        CHECK(d.kind == nx::diagnostic_kind::setup_error);
        CHECK(d.suggestion.empty()); // never a did-you-mean: the user typed nothing wrong
    }
}
} // namespace

TEST("args setup - two options cannot claim the same short name")
{
    auto force = false;
    auto fast = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");
    args.arg({"f", "fast"}, fast, "quickly");

    check_is_setup_error(args.validate_setup());
    CHECK(args.validate_setup().diagnostics()[0].message.contains("-f"));
}

TEST("args setup - the same long name twice is caught too")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.arg({"force"}, a, "do it");
    args.arg({"force"}, b, "again");

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - a negatable non-bool")
{
    auto jobs = 0;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .negatable = true});

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - required plus make_default is a contradiction")
{
    auto jobs = 0;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .required = true, .make_default = [] { return 8; }});

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - only one variadic positional")
{
    auto a = cc::vector<cc::string>();
    auto b = cc::vector<cc::string>();
    auto args = make_args();
    args.positional("A", a, {.desc = "first"});
    args.positional("B", b, {.desc = "second"});

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - only one rest binding")
{
    auto a = cc::vector<cc::string_view>();
    auto b = cc::vector<cc::string_view>();
    auto args = make_args();
    args.rest(a, "A");
    args.rest(b, "B");

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - a parse reports the declaration bug instead of the command line")
{
    auto force = false;
    auto fast = false;
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");
    args.arg({"f", "fast"}, fast, "quickly");

    // The command line here is also wrong, and that must NOT be what gets reported.
    auto const r = args.parse({"--nonsense"});
    check_is_setup_error(r);
}

TEST("args setup - a sound declaration passes")
{
    auto force = false;
    auto jobs = 4;
    auto files = cc::vector<cc::string>();
    auto tail = cc::vector<cc::string_view>();

    auto args = make_args();
    args.arg({"f", "force", nx::arg::hidden("legacy-force")}, force, "do it");
    args.arg({"j", "jobs"}, jobs, "how many");
    args.positional("FILES", files, {.desc = "inputs"});
    args.rest(tail, "ARGS");

    auto const r = args.validate_setup();
    CHECK(r.ok());
    CHECK(!r.has_diagnostics());
}

TEST("args setup - a setup error renders as internal, not as a usage complaint")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.arg({"f"}, a, "one");
    args.arg({"f"}, b, "two");

    auto const text = args.validate_setup().diagnostic_text({.color = false});
    CHECK(text.contains("internal error"));
    CHECK(!text.contains("did you mean"));
}

TEST("args setup - a default command that is not declared")
{
    auto args = make_args();
    args.command({"build"}, "build it", [](nx::args_builder&) {});
    args.default_command("biuld");

    // The old failure mode was silence: no command ran, no diagnostic, and the parse reported success.
    check_is_setup_error(args.validate_setup());
    check_is_setup_error(args.parse({}));
}

TEST("args setup - a default command that is a delegate")
{
    auto args = make_args();
    args.delegate({"external"}, "somebody else's parser", [](cc::span<cc::string_view const>) { return 0; });
    args.default_command("external");

    check_is_setup_error(args.validate_setup());
}

TEST("args setup - the root and its default command may not claim the same name")
{
    auto root_jobs = 1;
    auto sub_jobs = 1;

    auto args = make_args();
    args.arg({"j", "jobs"}, root_jobs, "how many, at the root");
    args.command({"build"}, "build it",
                 [&](nx::args_builder& sub) { sub.arg({"j", "jobs"}, sub_jobs, "how many, in the command"); });
    args.default_command("build");

    // An unnamed invocation hands the tail to the command, so -j would mean two different variables
    // depending on where the root's walk happened to stop.
    check_is_setup_error(args.validate_setup());
}

TEST("args setup - a name shared with a command that is NOT the default is fine")
{
    auto root_jobs = 1;
    auto sub_jobs = 1;

    auto args = make_args();
    args.arg({"j", "jobs"}, root_jobs, "how many, at the root");
    args.command({"build"}, "build it",
                 [&](nx::args_builder& sub) { sub.arg({"j", "jobs"}, sub_jobs, "how many, in the command"); });

    // Nothing is ambiguous while the command has to be spelled out: `t build -j 8` says which level it means.
    CHECK(args.validate_setup().ok());
    CHECK(args.parse({"build", "-j", "8"}).ok());
    CHECK(sub_jobs == 8);
    CHECK(root_jobs == 1);
}

TEST("args setup - global() on something that is not a named option")
{
    auto files = cc::vector<cc::string>();
    auto args = make_args();
    args.positional("FILES", files, {.desc = "inputs"});
    args.global();

    // There is no depth for a positional to be reachable at, so the call meant nothing and silently did
    // nothing, which is exactly the kind of declaration that should not survive a parse.
    check_is_setup_error(args.validate_setup());
}

TEST("args setup - an env fallback on a binding with nothing to parse into")
{
    auto ran = false;
    auto args = make_args();
    args.action({"go"}, [&ran] { ran = true; }, {.desc = "do it", .env = "T_GO"});

    // An action has no parse thunk, so the env value would have been read and dropped.
    check_is_setup_error(args.validate_setup());
    CHECK(!ran);
}
