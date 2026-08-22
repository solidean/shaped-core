#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Subcommands: a nested builder rather than a special case, declared lazily.
//
// The laziness is the part worth pinning hardest — a declare callback that runs twice, or runs when it
// should not have, is invisible until something doubles or a reference dangles.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t", .description = "a test program"});
    args.no_auto_print();
    return args;
}
} // namespace

TEST("args commands - the selected one runs and reports itself")
{
    auto jobs = 1;
    auto args = make_args();
    args.command({"build", "b"}, "build the project",
                 [&](nx::args_builder& sub) { sub.arg({"j", "jobs"}, jobs, "how many"); });
    args.command({"test"}, "run the tests", [](nx::args_builder&) {});

    SECTION("by its canonical name")
    {
        CHECK(args.parse({"build", "-j", "8"}).ok());
        CHECK(args.selected_command() == "build");
        CHECK(jobs == 8);
    }

    SECTION("by an alias, which still reports the canonical name")
    {
        CHECK(args.parse({"b", "-j", "4"}).ok());
        CHECK(args.selected_command() == "build");
        CHECK(jobs == 4);
    }

    SECTION("a sibling leaves the other alone")
    {
        CHECK(args.parse({"test"}).ok());
        CHECK(args.selected_command() == "test");
        CHECK(jobs == 1);
    }
}

TEST("args commands - a declare callback runs once, and only when it is needed")
{
    auto build_declared = 0;
    auto deploy_declared = 0;
    auto jobs = 1;

    auto args = make_args();
    args.command({"build"}, "build it",
                 [&](nx::args_builder& sub)
                 {
                     ++build_declared;
                     sub.arg({"j", "jobs"}, jobs, "how many");
                 });
    args.command({"deploy"}, "ship it", [&](nx::args_builder&) { ++deploy_declared; });

    CHECK(build_declared == 0); // nothing is declared until something asks
    CHECK(deploy_declared == 0);

    CHECK(args.parse({"build", "-j", "2"}).ok());
    CHECK(build_declared == 1);
    CHECK(deploy_declared == 0); // the sibling was never needed, so it never ran

    // Parsing the same command again must not declare its arguments a second time.
    CHECK(args.parse({"build", "-j", "3"}).ok());
    CHECK(build_declared == 1);
    CHECK(jobs == 3);
}

TEST("args commands - help forces every subtree, since it has to describe them")
{
    auto build_declared = 0;
    auto deploy_declared = 0;

    auto args = make_args();
    args.command({"build"}, "build it", [&](nx::args_builder&) { ++build_declared; });
    args.command({"deploy"}, "ship it", [&](nx::args_builder&) { ++deploy_declared; });

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("commands:"));
    CHECK(help.contains("build"));
    CHECK(help.contains("build it"));
    CHECK(help.contains("deploy"));
}

TEST("args commands - nesting")
{
    auto name = cc::string();
    auto args = make_args();
    args.command({"remote"}, "manage remotes",
                 [&](nx::args_builder& remote)
                 {
                     remote.command({"add"}, "add one",
                                    [&](nx::args_builder& add) { add.positional("NAME", name, {.desc = "which"}); });
                     remote.command({"remove"}, "drop one", [](nx::args_builder&) {});
                 });

    CHECK(args.parse({"remote", "add", "origin"}).ok());
    CHECK(name == "origin");

    REQUIRE(args.command_path().size() == 2);
    CHECK(args.command_path()[0] == "remote");
    CHECK(args.command_path()[1] == "add");
    CHECK(args.is_command("remote add"));
    CHECK(!args.is_command("remote"));
}

TEST("args commands - an unknown command suggests a real one")
{
    auto args = make_args();
    args.command({"build"}, "build it", [](nx::args_builder&) {});
    args.command({"deploy"}, "ship it", [](nx::args_builder&) {});

    auto const r = args.parse({"biuld"});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unknown_command);
    CHECK(r.diagnostics()[0].suggestion == "build");
}

TEST("args commands - a level with commands wants one")
{
    auto args = make_args();
    args.command({"build"}, "build it", [](nx::args_builder&) {});

    auto const r = args.parse({});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unknown_command);
}

TEST("args commands - unless a default was named")
{
    auto ran = cc::string();
    auto args = make_args();
    args.command({"build"}, "build it", [&](nx::args_builder&) {});
    args.default_command("build");

    CHECK(args.parse({}).ok());
    CHECK(args.selected_command() == "build");
}

TEST("args commands - a global option is accepted at any depth")
{
    auto verbose = false;
    auto jobs = 1;

    auto args = make_args();
    args.arg({"v", "verbose"}, verbose, "print more");
    args.global();
    args.command({"build"}, "build it", [&](nx::args_builder& sub) { sub.arg({"j", "jobs"}, jobs, "how many"); });

    SECTION("before the command name, as always")
    {
        CHECK(args.parse({"-v", "build"}).ok());
        CHECK(verbose);
    }

    SECTION("and after it, which is what global() buys")
    {
        CHECK(args.parse({"build", "-v", "-j", "2"}).ok());
        CHECK(verbose);
        CHECK(jobs == 2);
    }
}

TEST("args commands - a non-global parent option stays at its own level")
{
    auto quiet = false;
    auto args = make_args();
    args.arg({"q", "quiet"}, quiet, "say less");
    args.command({"build"}, "build it", [](nx::args_builder&) {});

    CHECK(args.parse({"-q", "build"}).ok());
    CHECK(!args.parse({"build", "-q"}).ok());
}

TEST("args commands - a child's own option wins over an inherited one of the same name")
{
    auto parent_output = cc::string("parent");
    auto child_output = cc::string("child");

    auto args = make_args();
    args.arg({"o", "output"}, parent_output, "where the parent writes");
    args.global();
    args.command({"build"}, "build it",
                 [&](nx::args_builder& sub) { sub.arg({"o", "output"}, child_output, "where the child writes"); });

    CHECK(args.parse({"build", "-o", "x"}).ok());
    CHECK(child_output == "x");
    CHECK(parent_output == "parent");
}

TEST("args commands - help <command> is the same as command --help")
{
    auto args = make_args();
    args.command({"build"}, "build it",
                 [](nx::args_builder& sub) { sub.info({.name = "t build", .description = "build it"}); });

    auto const r = args.parse({"help", "build"});
    CHECK(r.outcome() == nx::args_outcome::help_requested);
    CHECK(r.exit_code() == 0);
}

TEST("args commands - a delegate gets the tail untouched")
{
    auto seen = cc::vector<cc::string>();
    auto args = make_args();
    args.delegate({"external"}, "hand off to another tool",
                  [&](cc::span<cc::string_view const> tail)
                  {
                      for (auto const& t : tail)
                          seen.push_back(cc::string(t));

                      return 7;
                  });

    // --help and -- belong to whoever we handed off to, not to us.
    auto const r = args.parse({"external", "--help", "--", "-x"});
    CHECK(r.exit_code() == 7);

    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "--help");
    CHECK(seen[1] == "--");
    CHECK(seen[2] == "-x");
    CHECK(args.selected_command() == "external");
}

TEST("args commands - a delegate is listed but not described in detail")
{
    auto args = make_args();
    args.delegate({"external"}, "hand off", [](cc::span<cc::string_view const>) { return 0; });

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("external"));

    // Showing an empty option list for something we cannot introspect would be a lie.
    CHECK(help.contains("--help'"));
}

TEST("args commands - commands and positionals cannot share a level")
{
    auto file = cc::string();
    auto args = make_args();
    args.command({"build"}, "build it", [](nx::args_builder&) {});
    args.positional("FILE", file, {.desc = "which"});

    auto const r = args.validate_setup();
    CHECK(r.outcome() == nx::args_outcome::setup_error);
    REQUIRE(r.has_diagnostics());
    CHECK(r.diagnostics()[0].message.contains("both subcommands and positionals"));
}

TEST("args commands - two commands cannot claim the same name")
{
    auto args = make_args();
    args.command({"build"}, "one", [](nx::args_builder&) {});
    args.command({"build"}, "two", [](nx::args_builder&) {});

    CHECK(args.validate_setup().outcome() == nx::args_outcome::setup_error);
}

TEST("args commands - the usage line advertises that one is expected")
{
    auto args = make_args();
    args.command({"build"}, "build it", [](nx::args_builder&) {});

    CHECK(args.usage_line() == "usage: t <command>");
}
