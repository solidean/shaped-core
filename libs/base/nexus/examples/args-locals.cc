#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Binding a command line to local variables — the shape most programs want.
//
// Run it with the line it declares:      uv run dev.py example nexus/args-locals
// Or with your own:                      uv run dev.py example nexus/args-locals --test-args "--help"
//
// The declared line is what makes this demonstrate anything with nothing typed.
// Declaring one at all also matters: without it the example would be handed the harness's own --examples
// flag to parse, since a test that declares nothing falls through to the process's arguments.

EXAMPLE("nexus/args-locals", nx::config::args("--jobs 8 --verbose --output out.bin in.txt other.txt"))
{
    // The variables' own initializers ARE the defaults, which is why help can print what the program does.
    auto jobs = 4;
    auto verbose = false;
    auto color = true;
    auto output = cc::string("a.out");
    auto files = cc::vector<cc::string>();

    auto args = nx::args({
        .name = "args-locals",
        .description = "a made-up build tool, to show what one declaration produces",
        .version = "1.0",
    });

    args.arg({"j", "jobs"}, jobs, {.desc = "how many jobs to run at once", .env = "DEMO_JOBS"});
    args.arg({"v", "verbose"}, verbose, "print more");
    args.arg({"color"}, color, {.desc = "colorize the output", .negatable = true});

    args.group("output");
    args.arg({"o", "output"}, output, {.desc = "where to write the result", .metavar = "FILE"});
    args.positional("FILES", files, {.desc = "what to build", .min_count = 1});

    args.example("args-locals -j8 -o out.bin main.cc", "eight jobs, one input");

    // Nothing is printed by the parse itself: this example renders everything by hand so the output is
    // readable in one place.
    args.no_auto_print();

    auto const result = args.parse(nx::test_args());

    if (result.outcome() == nx::args_outcome::help_requested)
    {
        cc::println(args.help_text({.width = 96}));
        return;
    }

    if (!result.ok())
    {
        cc::println(result.diagnostic_text());
        cc::println(args.usage_line());
        return;
    }

    cc::println("parsed:");
    cc::println(cc::format("  jobs    {}", jobs));
    cc::println(cc::format("  verbose {}", verbose));
    cc::println(cc::format("  color   {}", color));
    cc::println(cc::format("  output  {}", output));

    auto listed = cc::string();
    for (auto const& file : files)
    {
        if (!listed.empty())
            listed += ", ";

        listed += file;
    }
    cc::println(cc::format("  files   {}", listed));

    // What a mistyped flag looks like, since that is most of what a parser is for.
    // Suggested, never applied: silently correcting a flag is how a script quietly does the wrong thing.
    cc::println("");
    cc::println("a typo, for comparison:");

    auto ignored_jobs = 4;
    auto typo = nx::args({.name = "args-locals"});
    typo.no_auto_print();
    typo.arg({"j", "jobs"}, ignored_jobs, "how many jobs to run at once");

    cc::println(typo.parse({"--jbos", "8"}).diagnostic_text());
}
