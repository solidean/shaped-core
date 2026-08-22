#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Subcommands, the way a git-shaped tool wants them.
//
//   uv run dev.py example nexus/args-commands
//   uv run dev.py example nexus/args-commands --test-args "remote add origin --verbose"
//   uv run dev.py example nexus/args-commands --test-args "--help"
//
// Two things this shows that are easy to miss:
//   a command's arguments are declared LAZILY, only when something needs them
//   the program keeps its own control flow — the query is the primitive, not a callback

namespace
{
int declare_count = 0;
} // namespace

EXAMPLE("nexus/args-commands", nx::config::args("build --jobs 8 src/main.cc"))
{
    auto verbose = false;

    auto jobs = 4;
    auto sources = cc::vector<cc::string>();

    auto remote_name = cc::string();
    auto remote_url = cc::string();

    auto args = nx::args({
        .name = "args-commands",
        .description = "a made-up multi-command tool",
        .version = "1.0",
    });
    args.no_auto_print();

    args.arg({"v", "verbose"}, verbose, "print more");
    args.global(); // ...so it is accepted after a command name too, not only before

    args.command({"build", "b"}, "compile the sources",
                 [&](nx::args_builder& sub)
                 {
                     ++declare_count;
                     sub.arg({"j", "jobs"}, jobs, {.desc = "how many jobs at once", .validate = nx::arg::at_least(1)});
                     sub.positional("SOURCES", sources, {.desc = "what to compile"});
                 });

    args.command({"remote"}, "manage remotes",
                 [&](nx::args_builder& remote)
                 {
                     ++declare_count;
                     remote.command({"add"}, "add a remote",
                                    [&](nx::args_builder& add)
                                    {
                                        add.positional("NAME", remote_name, {.desc = "what to call it"});
                                        add.positional("URL", remote_url, {.desc = "where it lives"});
                                    });
                     remote.command({"list"}, "show them all", [](nx::args_builder&) {});
                 });

    // A command this program does not own: everything after its name goes through untouched, --help included.
    args.delegate({"external"}, "hand off to another tool",
                  [](cc::span<cc::string_view const> tail)
                  {
                      cc::println(cc::format("  the other tool would receive {} argument(s)", tail.size()));
                      return 0;
                  });

    cc::println(cc::format("before parsing, {} command(s) have declared their arguments", declare_count));

    auto const result = args.parse(nx::current_args());

    if (result.outcome() == nx::args_outcome::help_requested)
    {
        cc::println(args.help_text({.width = 96}));
        return;
    }

    if (!result.ok())
    {
        cc::println(result.diagnostic_text());
        return;
    }

    // Only the command that ran declared anything.
    // Help and completion force the rest, because they have to describe commands this run will never touch.
    cc::println(cc::format("after parsing, {} have — only the path that was taken", declare_count));
    cc::println("");

    // Query, not a callback: the program decides what to do with its own control flow.
    if (args.is_command("build"))
    {
        cc::println(cc::format("build: {} job(s), {} source(s), verbose {}", jobs, sources.size(), verbose));
    }
    else if (args.is_command("remote add"))
    {
        cc::println(cc::format("remote add: {} -> {}", remote_name, remote_url));
    }
    else if (args.is_command("remote list"))
    {
        cc::println("remote list: nothing configured");
    }
    else
    {
        cc::println(cc::format("ran '{}'", args.selected_command()));
    }
}
