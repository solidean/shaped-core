#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// The ambient accessors.
//
// These run inside a nexus binary, so the captured argv is whatever `dev.py test` invoked — which is
// exactly the point of the last test here, and the reason none of the others assert on a specific value.

using namespace cc::primitive_defines;

TEST("args ambient - the program is identified, or honestly not")
{
    // nx::run captured argv, so this binary knows its own name.
    // A binary that never went through the harness falls back to the OS, and a platform that cannot answer
    // yields empty rather than asserting.
    CHECK(!nx::program_path().empty());
    CHECK(nx::program_name() == "nexus-test");

    // The basename, with no directory and no packaging suffix, whatever the platform spells it as — which
    // is why the expectation above can be one literal on all of them.
    CHECK(!nx::program_name().contains('/'));
    CHECK(!nx::program_name().contains('\\'));
    CHECK(!nx::program_name().contains(".exe"));
    CHECK(!nx::program_name().contains(".js"));
}

TEST("args ambient - argv[0] is not one of the arguments")
{
    for (auto const& arg : nx::process_args())
        CHECK(arg != nx::program_path());
}

TEST("args ambient - the two sources are separate, with no fallback between them")
{
    // This test declares no arguments of its own, so test_args() is empty — it does not quietly become the
    // process's, which is how a test ends up trying to parse the harness's flags.
    CHECK(nx::test_args().empty());
    CHECK(!nx::process_args().empty());
}

TEST("args ambient - the undeclared accessors never fail")
{
    // Whatever this binary was invoked with, none of these may assert or throw.
    CHECK(!nx::has_arg("--definitely-not-given-anywhere"));
    CHECK(!nx::get_arg("definitely-not-given-anywhere").has_value());
    CHECK(!nx::get_arg<int>("definitely-not-given-anywhere").has_value());

    // A value that is present but unparseable as the requested type is simply absent: a debug helper that
    // explained itself would invite being depended on.
    CHECK(!nx::get_arg<int>("--not-a-flag-here").has_value());
}

TEST("args ambient - a name may be written with or without dashes")
{
    // All three spellings resolve to the same lookup, so a caller need not remember which form to use.
    CHECK(nx::has_arg("junit-xml") == nx::has_arg("--junit-xml"));
    CHECK(nx::has_arg("v") == nx::has_arg("-v"));
}

TEST("args ambient - the lenient grammar, over a command line nobody declared")
{
    // The accessors read whatever the process was given, which a test cannot control — so the grammar
    // itself is pinned through a builder that CAN be handed a known line, using the same rules.
    auto seen = cc::vector<cc::string>();
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.value_action({"n", "name"}, [&](cc::string_view v) { seen.push_back(cc::string(v)); });

    CHECK(args.parse({"--name=inline"}).ok());
    CHECK(args.parse({"--name", "spaced"}).ok());

    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "inline");
    CHECK(seen[1] == "spaced");
}

TEST("args ambient - repeated capture is a no-op rather than an assertion")
{
    // A nexus meta-test nests a whole run inside a running one, so this happens for real.
    char const* argv[] = {"other.exe", "--something"};
    nx::impl::set_process_args(2, argv);

    // The outer process's arguments are still the true ones.
    CHECK(nx::program_name() == "nexus-test");
}
