#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh>

// Apps and commands: the programs a nexus binary carries beside its tests, and how a command line picks one.

namespace nx
{
struct test_registry;
struct test_declaration;

/// Run the COMMAND named `name` in this binary with `args` as its command line, and return the status it exited with.
///
/// Runs it as a dispatched child of the calling test, reported under it, so its checks count and a failure shows.
/// Its baked-in `main_thread` and `exclusive()` are the calling test's to hold, as for a synchronous invocable.
/// A failed check turns a zero status into 1.
int run_command(cc::string_view name, cc::vector<cc::string> args = {});
} // namespace nx

namespace nx::impl
{
/// What a command line asks a nexus binary to do.
enum class entry_route_kind
{
    tests,    // parse it as nexus's own: a nexus selector was given, or a test was named exactly
    entry,    // run one app or command, with the rest of the line as its arguments
    overview, // nothing was selected and there is no default: list what the binary holds
    error,    // something was named that the binary does not hold
};

struct entry_route
{
    entry_route_kind kind = entry_route_kind::tests;
    test_declaration const* entry = nullptr;
    cc::vector<cc::string> entry_args;
    cc::string message;
};

/// Decides what `args` (argv without the program) selects, name first.
///
/// An app or command named by the first token runs with the rest.
/// A nexus selector anywhere (--tests, --examples, --benchmarks, --pgo-benchmarks, --manual, --apps, --commands,
/// --list-tests, --list-tests-json, --reporter) hands the line to nexus, as does a test named exactly, or a bare
/// --help when there is no default.
/// Otherwise the default app or command takes the whole line, and with none an empty line is the overview and
/// anything else is an error.
entry_route route_command_line(test_registry const& registry, cc::span<cc::string_view const> args);

/// Every problem with the binary's default entries, empty when there is none.
/// More than one default, or a default on anything but an app or a command, breaks every run.
cc::string check_default_entries(test_registry const& registry);

/// What a binary holds, for a run with nothing selected: its apps, commands, examples and benchmarks by name, its
/// tests by count, and how to run each.
cc::string render_overview(test_registry const& registry, cc::string_view program);

/// Records the exit status the running command's body returned.
void report_exit_code(int code);
} // namespace nx::impl
