#include "options.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/args.hh>

// The richest CLI in the repo, declared once.
//
// Two of its flags take a comma-separated set rather than a scalar, so each is a value TYPE with its own
// trait: the accepted names are then listed in help and offered by completion, instead of living in a
// hand-written table that a new section has to be remembered into.

/// `--sections trace,stats,...` — the set, not one of them.
template <>
struct nx::custom::arg_value_trait<itrace::output_sections>
{
    static bool parse(cc::string_view token, itrace::output_sections& out, cc::string& error);
    static cc::string_view type_name() { return "LIST"; }
    static void values(cc::vector<cc::string>& out)
    {
        for (auto const* const name : {"trace", "stats", "memory", "cachelines", "memory-stats", "timing"})
            out.push_back(cc::string(name));
    }
};

/// `--memory-regions heap,stack,...` — likewise, and an explicit list REPLACES the default rather than
/// adding to it, so the first token seen clears what the struct came with.
template <>
struct nx::custom::arg_value_trait<itrace::memory_regions>
{
    static bool parse(cc::string_view token, itrace::memory_regions& out, cc::string& error);
    static cc::string_view type_name() { return "LIST"; }
    static void values(cc::vector<cc::string>& out)
    {
        for (auto const* const name : {"heap", "frame", "stack", "instructions"})
            out.push_back(cc::string(name));
    }
};

namespace itrace
{
namespace
{
/// Apply `fn` to each comma-separated token of `value`.
/// Empty tokens are skipped, so "a,,b" and a trailing comma are tolerated.
template <class F>
bool for_each_token(cc::string_view value, F&& fn)
{
    auto start = isize(0);
    while (start <= value.size())
    {
        auto const comma = value.find(',', start);
        auto const end = comma < 0 ? value.size() : comma;
        if (end > start && !fn(value.subview({.start = start, .end = end})))
            return false;

        if (comma < 0)
            break;

        start = comma + 1;
    }

    return true;
}
} // namespace
} // namespace itrace

bool nx::custom::arg_value_trait<itrace::output_sections>::parse(cc::string_view token,
                                                                 itrace::output_sections& out,
                                                                 cc::string& error)
{
    return itrace::for_each_token(token,
                                  [&](cc::string_view name)
                                  {
                                      if (name == "trace")
                                          out.trace = true;
                                      else if (name == "stats")
                                          out.stats = true;
                                      else if (name == "memory")
                                          out.memory = true;
                                      else if (name == "cachelines")
                                          out.cachelines = true;
                                      else if (name == "memory-stats")
                                          out.memory_stats = true;
                                      else if (name == "timing")
                                          out.timing = true;
                                      else
                                      {
                                          error = cc::format("unknown section '{}'", name);
                                          return false;
                                      }

                                      return true;
                                  });
}

bool nx::custom::arg_value_trait<itrace::memory_regions>::parse(cc::string_view token,
                                                                itrace::memory_regions& out,
                                                                cc::string& error)
{
    // An explicit list replaces the default, so start from nothing selected.
    out = {.heap = false, .frame = false, .stack = false, .instructions = false};

    return itrace::for_each_token(token,
                                  [&](cc::string_view name)
                                  {
                                      if (name == "heap")
                                          out.heap = true;
                                      else if (name == "frame")
                                          out.frame = true;
                                      else if (name == "stack")
                                          out.stack = true;
                                      else if (name == "instructions")
                                          out.instructions = true;
                                      else
                                      {
                                          error = cc::format("unknown memory region '{}'", name);
                                          return false;
                                      }

                                      return true;
                                  });
}

namespace itrace
{
namespace
{
/// The whole CLI, shared by the parse and by --help so neither can describe what the other does not.
nx::args_builder build_cli(options& opts, cc::vector<cc::string_view>& debuggee_args, cc::string& target_error)
{
    auto args = nx::args({
        .name = "instruction-tracer",
        .description = "record what optimized x64 code actually executed (Windows only)",
        .help = "Every boolean flag has a --no-<flag> form, e.g. --no-source.\n"
                "All sections come from one capture, since memory data cannot be reliably reproduced across runs.",
    });

    args.arg({"exe"}, opts.exe, {.desc = "the binary to trace", .metavar = "PATH", .required = true});

    args.group("target (exactly one)");

    // Three spellings of one setting, so each writes it its own way and a document rule keeps them apart.
    args.value_action({"symbol"},
                      [&opts](cc::string_view value)
                      {
                          // An explicit --symbol is always a symbol, even when it looks like an address.
                          opts.target.form = target_spec::kind::symbol;
                          opts.target.symbol = value;
                      },
                      {.desc = "break on a symbol; a unique substring is enough", .metavar = "NAME"});

    args.value_action({"address"},
                      [&opts, &target_error](cc::string_view value)
                      {
                          auto address = parse_address(value);
                          if (address.has_error())
                          {
                              target_error = address.error().to_string();
                              return;
                          }

                          opts.target.form = target_spec::kind::address;
                          opts.target.address = address.value();
                      },
                      {.desc = "break on an absolute runtime address, e.g. 0x7ff611203410", .metavar = "HEX"});

    args.value_action({"target"},
                      [&opts, &target_error](cc::string_view value)
                      {
                          auto spec = parse_target_spec(value);
                          if (spec.has_error())
                          {
                              target_error = spec.error().to_string();
                              return;
                          }

                          opts.target = cc::move(spec.value());
                      },
                      {.desc = "one of: foo::bar | 0x7ff6... | mod.exe!foo::bar | mod.exe+0x3410", .metavar = "SPEC"});

    args.mutually_exclusive({"--symbol", "--address", "--target"});
    args.at_least_one_of({"--symbol", "--address", "--target"});

    args.group("collection");
    args.arg({"skip"}, opts.skip, {.desc = "ignore the first n entry hits", .metavar = "N"});
    args.arg({"traces"}, opts.traces, {.desc = "record n invocations", .metavar = "N", .validate = nx::arg::at_least(1)});
    args.arg({"instructions"}, opts.instructions,
             {.desc = "max retired instructions per trace", .metavar = "N", .validate = nx::arg::at_least(1)});
    args.arg({"until-return"}, opts.until_return, {.desc = "stop once the entry frame returns", .negatable = true});
    args.arg({"stop-at-syscall"}, opts.stop_at_syscall, {.desc = "stop before executing a syscall", .negatable = true});

    args.group("output sections (combine freely; all come from one capture)");
    args.arg({"sections"}, opts.sections, {.desc = "which sections to print", .metavar = "LIST"});
    args.arg({"stats"}, opts.sections.stats, {.desc = "shortcut for --sections stats", .negatable = true});
    args.arg({"html"}, opts.html_path,
             {.desc = "write a self-contained HTML report here; forces a full capture and the larger budget",
              .metavar = "PATH"});

    args.group("timing (llvm-mca cost model)");
    args.arg(
        {"mca"}, opts.mca_tool,
        {.desc = "path to llvm-mca; enables the timing section. Absent, timing degrades to nothing", .metavar = "PATH"});
    args.arg({"mca-cpu"}, opts.mca_cpu,
             {.desc = "micro-arch to model (default: host via -mcpu=native)", .metavar = "NAME"});

    args.group("trace section");
    args.arg({"stack"}, opts.stack, {.desc = "print the stack at entry", .negatable = true});
    args.arg({"source"}, opts.source, {.desc = "annotate with source file/line and text", .negatable = true});
    args.arg({"register-diffs"}, opts.register_diffs,
             {.desc = "show registers changed by each instruction", .negatable = true});

    args.group("memory sections");
    args.arg({"memory-regions"}, opts.regions,
             {.desc = "which address regions to include; frame is this function's own stack, stack another's, "
                      "instructions the code fetches",
              .metavar = "LIST"});
    args.arg({"memory-instruction-addresses"}, opts.memory_instruction_addresses,
             {.desc = "annotate accesses with the accessing rip", .negatable = true});

    args.group("process");
    args.arg({"terminate-after-traces"}, opts.terminate_after_traces,
             {.desc = "kill the debuggee once done", .negatable = true});

    args.group("output");
    args.action({"colored"}, [&opts] { opts.color = cc::console::color_mode::always; }, "force color on");
    args.action({"plain"}, [&opts] { opts.color = cc::console::color_mode::never; }, "force color off");

    args.rest(debuggee_args, "ARGS", "everything after -- goes to the debuggee verbatim");

    args.section("the instruction budget",
                 "Any non-trace section, and --html, raise the --instructions default to 100000: a truncated trace "
                 "makes every aggregate silently wrong, and 100 would truncate anything worth tabling.\n"
                 "An explicit --instructions is always respected, whichever order the flags came in.");

    args.no_auto_print();
    args.no_auto_completion();
    return args;
}
} // namespace

cc::result<options, cc::string> parse_options(cc::span<char const* const> args)
{
    auto opts = options();
    auto debuggee_args = cc::vector<cc::string_view>();
    auto target_error = cc::string();

    auto parser = build_cli(opts, debuggee_args, target_error);

    // argv[0] is the program itself.
    auto tokens = cc::vector<cc::string_view>();
    for (auto i = isize(1); i < args.size(); ++i)
        tokens.push_back(cc::string_view(args[i]));

    auto const result = parser.parse(tokens);

    if (result.outcome() == nx::args_outcome::help_requested)
    {
        opts.help = true;
        return opts;
    }

    // A target spec that did not parse is reported by the action that read it, since the value is valid
    // text and only this program knows what shape it must have.
    if (!target_error.empty())
        return cc::error(target_error);

    if (!result.ok())
    {
        auto message = cc::string();
        for (auto const& diagnostic : result.diagnostics())
        {
            if (!message.empty())
                message += "\n";

            message += diagnostic.message;
            if (!diagnostic.suggestion.empty())
                cc::format_append(message, " (did you mean {}?)", diagnostic.suggestion);
        }

        return cc::error(message);
    }

    for (auto const& arg : debuggee_args)
        opts.target_args.push_back(cc::string(arg));

    // Either spelling of the shortcut is an explicit choice about sections, including --no-stats.
    opts.sections_explicit = parser.was_given("--sections") || parser.was_given("--stats");

    // No section selected means the trace alone.
    if (opts.sections.none())
        opts.sections.trace = true;

    // Order-independent: the cap is raised only where the user set none, whichever flag came first.
    // The HTML export bundles every aggregate, so it takes the same budget.
    if ((opts.sections.any_non_trace() || !opts.html_path.empty()) && !parser.was_given("--instructions"))
        opts.instructions = stats_instruction_default;

    return opts;
}

cc::string_view usage_text()
{
    // Rendered once and kept, because the declared return type is a view.
    static auto const text = []
    {
        auto opts = options();
        auto debuggee_args = cc::vector<cc::string_view>();
        auto target_error = cc::string();
        auto args = build_cli(opts, debuggee_args, target_error);

        return args.help_text({.width = cc::console::terminal_width().value_or(100)});
    }();

    return text;
}
} // namespace itrace
