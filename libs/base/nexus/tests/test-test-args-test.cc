#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <nexus/tests/schedule.hh>

// A test's own command line: declared with nx::config::args, replaced by --test-args, and read back from
// inside the body through nx::current_args().
//
// The first few run their assertions on themselves, which is the only honest way to check that the ambient
// chain really delivers to a running body.

using namespace cc::primitive_defines;

namespace
{
/// One config parsed from a literal command line, argv[0] included as a real invocation has it.
nx::test_schedule_config parse(cc::span<char const* const> args)
{
    auto argv = cc::vector<char const*>();
    argv.push_back("nexus-test.exe");
    for (auto const* const a : args)
        argv.push_back(a);

    return nx::test_schedule_config::create_from_args(int(argv.size()), const_cast<char**>(argv.data()));
}

/// The current test's arguments, joined, so an expectation reads as one string.
cc::string joined_args()
{
    auto out = cc::string();
    for (auto const& arg : nx::current_args())
    {
        if (!out.empty())
            out += "|";

        out += arg;
    }

    return out;
}
} // namespace

TEST("test args - a declared line reaches the body", nx::config::args("--jobs 8 --verbose"))
{
    CHECK(joined_args() == "--jobs|8|--verbose");
}

TEST("test args - quoting follows the shared tokenizer rules", nx::config::args("--name \"two words\" --path 'c:\\temp'"))
{
    auto const args = nx::current_args();
    REQUIRE(args.size() == 4);
    CHECK(args[1] == "two words");
    CHECK(args[3] == "c:\\temp"); // single quotes keep a Windows path intact
}

TEST("test args - a test that declares none sees the process's own")
{
    // Not the empty list: outside a declared line, current_args falls through to the process, which is what
    // makes a helper written for a tool keep working when it is called from inside a test.
    CHECK(nx::current_args().size() == nx::process_args().size());
}

TEST("test args - the declared line is what nx::args parses", nx::config::args("--jobs 12 extra"))
{
    auto jobs = 1;
    auto rest = cc::vector<cc::string>();

    auto args = nx::args({.name = "inner"});
    args.no_auto_print();
    args.arg({"j", "jobs"}, jobs, "how many");
    args.positional("REST", rest, {.desc = "whatever else"});

    // The whole point: a test or example parses its own command line with the same machinery a tool does.
    REQUIRE(args.parse(nx::current_args()).ok());
    CHECK(jobs == 12);
    REQUIRE(rest.size() == 1);
    CHECK(rest[0] == "extra");
}

TEST("test args - an explicitly empty line is not the same as none", nx::config::args(""))
{
    // `""` was declared, so the test sees nothing rather than falling through to the process's arguments.
    CHECK(nx::current_args().empty());
}

// --- how the run's own flags interact with a declared line ---------------------------------------------

TEST("test args - the CLI form replaces the declared one")
{
    // Replacement, never merging: two argument lines cannot be combined into one that means anything.
    auto const config = parse({"--test-args", "--replaced"});
    REQUIRE(config.test_args.size() == 1);
    CHECK(config.test_args[0] == "--replaced");
}

TEST("test args - a run-wide line applies to every selected test")
{
    // Worth knowing rather than surprising: --test-args is one line for the whole run, while
    // nx::config::args is per test.
    auto const config = parse({"--test-args", "--shared"});
    auto& registry = nx::get_static_test_registry();
    auto const schedule = nx::test_schedule::create(config, registry);

    auto checked = 0;
    for (auto const& instance : schedule.instances)
    {
        REQUIRE(instance.args.size() == 1);
        CHECK(instance.args[0] == "--shared");
        ++checked;
    }

    CHECK(checked > 1);
}

TEST("test args - a declared line reaches the schedule when the CLI gives none")
{
    auto& registry = nx::get_static_test_registry();
    auto const config = parse({"test args - a declared line reaches the body"});
    auto const schedule = nx::test_schedule::create(config, registry);

    REQUIRE(schedule.instances.size() == 1);
    REQUIRE(schedule.instances[0].args.size() == 3);
    CHECK(schedule.instances[0].args[0] == "--jobs");
}

TEST("test args - the views a body reads point at the instance's own storage")
{
    auto& registry = nx::get_static_test_registry();
    auto const config = parse({"test args - a declared line reaches the body"});
    auto const schedule = nx::test_schedule::create(config, registry);

    REQUIRE(schedule.instances.size() == 1);
    auto const& instance = schedule.instances[0];

    // Built after the vector stopped growing, which is the one ordering that keeps them valid — cc::string's
    // small-string optimization makes a view taken too early dangle silently.
    REQUIRE(instance.arg_views.size() == instance.args.size());
    for (auto i = isize(0); i < instance.args.size(); ++i)
        CHECK(instance.arg_views[i] == cc::string_view(instance.args[i]));
}
