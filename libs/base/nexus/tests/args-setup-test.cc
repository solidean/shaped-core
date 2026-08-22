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
