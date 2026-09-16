#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/ambient.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/entry.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// Apps, commands, and how a command line picks between them and the tests.
// The routing is checked against local registries, so nothing here depends on what this binary happens to hold.

using namespace cc::primitive_defines;

namespace
{
using route_kind = nx::impl::entry_route_kind;

nx::config::cfg entry_cfg(nx::config::test_bucket bucket, bool is_default = false)
{
    auto cfg = nx::config::cfg{};
    cfg.bucket = bucket;
    cfg.default_entry = is_default;
    return cfg;
}

nx::impl::entry_route route(nx::test_registry const& reg, cc::vector<cc::string_view> args)
{
    return nx::impl::route_command_line(reg, args);
}

/// A tool binary: a default command, a second command, an app, and a test.
nx::test_registry tool_registry()
{
    auto reg = nx::test_registry();
    reg.add_declaration("lint", entry_cfg(nx::config::test_bucket::command, true), [] {});
    reg.add_declaration("format", entry_cfg(nx::config::test_bucket::command), [] {});
    reg.add_declaration("viewer", entry_cfg(nx::config::test_bucket::app), [] {});
    reg.add_declaration("lint - a clean file", {}, [] { CHECK(true); });
    return reg;
}
} // namespace

TEST("entry - a command named first runs with the rest of the line")
{
    auto const reg = tool_registry();
    auto const r = route(reg, {"format", "--in-place", "a.cc"});
    REQUIRE(r.kind == route_kind::entry);
    CHECK(r.entry->name == "format");
    REQUIRE(r.entry_args.size() == 2);
    CHECK(r.entry_args[0] == "--in-place");
}

TEST("entry - with a default, a line naming nothing is the default's, flags and all")
{
    auto const reg = tool_registry();

    auto const bare = route(reg, {});
    REQUIRE(bare.kind == route_kind::entry);
    CHECK(bare.entry->name == "lint");

    // nexus's own -j and --help belong to the tool here: nothing nexus-shaped selected them.
    auto const flags = route(reg, {"-j", "4", "--help", "src/a.cc"});
    REQUIRE(flags.kind == route_kind::entry);
    CHECK(flags.entry->name == "lint");
    CHECK(flags.entry_args.size() == 4);
}

TEST("entry - a nexus selector, or a test named exactly, hands the line to nexus")
{
    auto const reg = tool_registry();
    CHECK(route(reg, {"--tests"}).kind == route_kind::tests);
    CHECK(route(reg, {"--tests", "lint - a"}).kind == route_kind::tests);
    CHECK(route(reg, {"-j", "4", "--examples"}).kind == route_kind::tests);
    CHECK(route(reg, {"--list-tests-json=-"}).kind == route_kind::tests);
    CHECK(route(reg, {"--reporter", "xml"}).kind == route_kind::tests); // Catch2 callers never say --tests
    CHECK(route(reg, {"lint - a clean file"}).kind == route_kind::tests);

    // --tests first means a test named like a command is still the test.
    CHECK(route(reg, {"--tests", "lint"}).kind == route_kind::tests);
}

TEST("entry - without a default, nothing is the overview and an unknown name is an error")
{
    auto reg = nx::test_registry();
    reg.add_declaration("vector - grows", {}, [] { CHECK(true); });

    CHECK(route(reg, {}).kind == route_kind::overview);
    CHECK(route(reg, {"--help"}).kind == route_kind::tests); // nexus's own help, since no default owns it

    auto const unknown = route(reg, {"vector"}); // a substring filter needs --tests now
    REQUIRE(unknown.kind == route_kind::error);
    CHECK(unknown.message.contains("--tests"));
}

TEST("entry - more than one default, or a default that is no program, breaks the binary")
{
    auto ok = tool_registry();
    CHECK(nx::impl::check_default_entries(ok).empty());

    auto two = tool_registry();
    two.add_declaration("second", entry_cfg(nx::config::test_bucket::app, true), [] {});
    CHECK(nx::impl::check_default_entries(two).contains("more than one"));

    auto wrong = nx::test_registry();
    wrong.add_declaration("a test", entry_cfg(nx::config::test_bucket::normal, true), [] { CHECK(true); });
    CHECK(nx::impl::check_default_entries(wrong).contains("only an APP or a COMMAND"));
}

TEST("entry - the overview names programs, counts tests, and elides a long list")
{
    auto reg = tool_registry();
    for (auto i = 0; i < 12; ++i)
        reg.add_declaration(cc::format("examples/e{:02}", i), entry_cfg(nx::config::test_bucket::example), [] {});

    auto const text = nx::impl::render_overview(reg, "tool");
    CHECK(text.contains("2 command(s), 1 app(s), 12 example(s)"));
    CHECK(text.contains("lint, format"));
    CHECK(text.contains("and 4 more"));
    CHECK(text.contains("--tests"));
    CHECK(!text.contains("lint - a clean file")); // tests are counted, never listed
}

namespace
{
int exits_three()
{
    CHECK(true);
    return 3;
}

cc::shared_async<int> exits_five_later()
{
    auto const later = cc::make_async_lazy([] { return 5; });
    co_return co_await later;
}
} // namespace

TEST("entry - a command's exit status is what its body returned, sync or async", no_scheduler)
{
    auto reg = nx::test_registry();
    reg.add_declaration("three", entry_cfg(nx::config::test_bucket::command),
                        [] { nx::impl::report_exit_code(exits_three()); });
    reg.add_async_declaration("five", entry_cfg(nx::config::test_bucket::command), [](nx::impl::async_test_sink& sink)
                              { nx::impl::submit_command_async(sink, exits_five_later()); });

    auto select = nx::test_schedule_config{};
    select.selected_bucket = nx::config::test_bucket::command;
    auto const schedule = nx::test_schedule::create(select, reg);
    auto const exec = nx::execute_tests(schedule, {});

    REQUIRE(exec.executions.size() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.executions[0].exit_code.value_or(-1) == 3);
    CHECK(exec.executions[1].exit_code.value_or(-1) == 5);
}

namespace
{
// Returns at the end rather than inside a section: a return ends the pass before the next section is discovered.
cc::shared_async<int> exits_per_section()
{
    auto status = 0;
    SECTION("one")
    {
        CHECK(true);
        status = 1;
    }
    SECTION("two")
    {
        CHECK(true);
        status = 2;
    }
    co_return status;
}
} // namespace

TEST("entry - an async command with sections exits with the status of its last pass, as a sync one does", no_scheduler)
{
    auto reg = nx::test_registry();
    reg.add_async_declaration("sections", entry_cfg(nx::config::test_bucket::command), [](nx::impl::async_test_sink& sink)
                              { nx::impl::submit_command_async(sink, exits_per_section()); });

    auto select = nx::test_schedule_config{};
    select.selected_bucket = nx::config::test_bucket::command;
    auto const schedule = nx::test_schedule::create(select, reg);
    auto const exec = nx::execute_tests(schedule, {});

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.executions[0].root.subsections.size() == 2);
    CHECK(exec.executions[0].exit_code.value_or(-1) == 2);
}

TEST("entry - run_command runs a command as a child and hands back its status", no_scheduler)
{
    auto reg = nx::test_registry();
    reg.add_declaration("echo-count", entry_cfg(nx::config::test_bucket::command),
                        []
                        {
                            CHECK(true);
                            nx::impl::report_exit_code(int(nx::test_args().size()));
                        });

    auto code = -1;
    // The command bakes in nothing here, so its caller needs to hold nothing either.
    reg.add_declaration("caller", {}, [&] { code = nx::run_command("echo-count", {"a", "b", "c"}); });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, {});

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(code == 3); // it saw exactly the command line it was given
    REQUIRE(exec.executions[0].nested.size() == 1);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "echo-count");
}

// The macros end to end, in the static registry: none of these is swept by a test run, which is part of the point.
COMMAND("nexus/echo-status")
{
    // Exits with the number its first argument spells, so a caller can see the line arrived.
    auto const args = nx::test_args();
    CHECK(true);
    return args.empty() ? 0 : int(args[0].size());
}

ASYNC_COMMAND("nexus/async-echo-status")
{
    auto const later = cc::make_async_lazy([] { return 9; });
    co_return co_await later;
}

APP("nexus/app-wiring")
{
    CHECK(true);
}

ASYNC_APP("nexus/async-app-wiring")
{
    CHECK(true);
    co_return;
}

TEST("entry - the macros declare their buckets, bake in main_thread and exclusive(), and stay out of a sweep")
{
    auto const find = [](cc::string_view name) -> nx::test_declaration const*
    {
        for (auto const& decl : nx::get_static_test_registry().declarations)
            if (cc::string_view(decl.name) == name)
                return &decl;
        return nullptr;
    };

    for (auto const* name :
         {"nexus/echo-status", "nexus/async-echo-status", "nexus/app-wiring", "nexus/async-app-wiring"})
    {
        auto const* const decl = find(name);
        REQUIRE(decl != nullptr);
        CHECK(decl->test_config.main_thread);
        CHECK(decl->test_config.exclusive_global);
        CHECK(!nx::test_schedule_config{}.would_run(*decl));
    }
    CHECK(find("nexus/echo-status")->test_config.bucket == nx::config::test_bucket::command);
    CHECK(find("nexus/async-echo-status")->is_async());
    CHECK(find("nexus/app-wiring")->test_config.bucket == nx::config::test_bucket::app);
}

// A command bakes in main_thread and exclusive(), and run_command runs it in this test's slot, so this test holds both.
TEST("entry - run_command runs a macro-declared COMMAND with its own command line", main_thread, exclusive())
{
    CHECK(nx::run_command("nexus/echo-status", {"seven"}) == 5);
    CHECK(nx::run_command("nexus/echo-status") == 0);
}
