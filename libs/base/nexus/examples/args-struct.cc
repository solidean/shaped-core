#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// The by-value path: a struct that declares itself, filled in by one call.
//
//   uv run dev.py example nexus/args-struct
//   uv run dev.py example nexus/args-struct --test-args "--jobs 999"
//
// Worth choosing over locals when the options are worth a name — passed to a function, stored, or shared
// between a tool and its tests.

namespace
{
struct build_options
{
    // The member initializers ARE the defaults, exactly as a local's initializer is on the other path.
    int jobs = 4;
    bool verbose = false;
    cc::string output = "a.out";
    cc::vector<cc::string> files;

    // The tier for a type you own.
    // A type you cannot change is adapted with nx::custom::args_trait<T> instead, which is checked first.
    static void declare_args(nx::args_builder& args, build_options& self)
    {
        args.info({
            .name = "args-struct",
            .description = "the same made-up build tool, declared as a struct",
            .version = "1.0",
        });

        args.arg({"j", "jobs"}, self.jobs,
                 {.desc = "how many jobs to run at once", .validate = nx::arg::in_range(1, 64)});
        args.arg({"v", "verbose"}, self.verbose, "print more");
        args.arg({"o", "output"}, self.output, {.desc = "where to write the result", .metavar = "FILE"});
        args.positional("FILES", self.files, {.desc = "what to build"});

        args.no_auto_print();
    }
};

void describe(build_options const& options)
{
    cc::println(cc::format("  jobs    {}", options.jobs));
    cc::println(cc::format("  verbose {}", options.verbose));
    cc::println(cc::format("  output  {}", options.output));
    cc::println(cc::format("  files   {}", options.files.size()));
}
} // namespace

EXAMPLE("nexus/args-struct", nx::config::args("--jobs 8 main.cc"))
{
    auto const parsed = nx::parse_args<build_options>(nx::current_args());

    // Three states, not two: --help is neither success nor failure, and a bool return could not say so.
    if (parsed.should_exit())
    {
        cc::println(cc::format("would exit with {}", parsed.exit_code()));
        cc::println(parsed.result().diagnostic_text());
        return;
    }

    cc::println("parsed:");
    describe(parsed.value());

    // Validation lives with the declaration, so the rule that rejects a value is the rule help prints.
    cc::println("");
    cc::println("out of range, for comparison:");

    auto const rejected = nx::parse_args<build_options>({"--jobs", "999"});
    cc::println(rejected.result().diagnostic_text());
}
