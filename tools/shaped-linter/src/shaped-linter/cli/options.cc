#include "options.hh"

#include <clean-core/string/format.hh>
#include <nexus/args/args.hh>

// Four verbs, four declarations.
//
// The verbs are still dispatched by hand in main.cc rather than declared as nx::args subcommands, and that
// is deliberate: `shaped-linter foo.cc` and `shaped-linter prose apply <plan>` mean a bare word is either
// a file or a verb, which is exactly the ambiguity a command level may not carry.
//
// Each usage text below is GENERATED from the declaration it documents, so a flag cannot be described
// without being parsed, or parsed without being described.

namespace scl
{
namespace
{
/// Every verb takes the same two colour flags, and `--no-color` is kept because scripts still write it.
void add_color_options(nx::args_builder& args, cc::console::color_mode& color)
{
    args.arg({"color"}, color, {.desc = "when to colorize; auto only on a terminal", .metavar = "MODE"});
    args.action({"no-color"}, [&color] { color = cc::console::color_mode::never; }, "the old spelling of --color never");
}

/// main prints the usage text itself, so the parse only reports.
void finish(nx::args_builder& args)
{
    args.no_auto_print();
    args.no_auto_completion();
}

/// What every entry point does with a parse: help short-circuits, a failure becomes the result's error.
/// `handled` says the caller should return `opts` as-is.
struct outcome
{
    bool handled = false;
    bool failed = false;
    cc::string message;
};

outcome interpret(nx::args_result const& result, bool& help_flag)
{
    if (result.outcome() == nx::args_outcome::help_requested)
    {
        help_flag = true;
        return {.handled = true};
    }

    if (!result.ok())
    {
        // The messages WITHOUT the "error: " label nx::args would render: main prefixes its own, and a
        // cc::result carries a reason rather than a rendered report.
        auto message = cc::string();
        for (auto const& diagnostic : result.diagnostics())
        {
            if (!message.empty())
                message += "\n";

            message += diagnostic.message;
            if (!diagnostic.suggestion.empty())
                cc::format_append(message, " (did you mean {}?)", diagnostic.suggestion);
        }

        return {.handled = true, .failed = true, .message = cc::move(message)};
    }

    return {};
}

/// argv[0] is the program itself, and only the root verb is handed it.
cc::vector<cc::string_view> tokens_of(cc::span<char const* const> args, isize skip)
{
    auto out = cc::vector<cc::string_view>();
    for (auto i = skip; i < args.size(); ++i)
        out.push_back(cc::string_view(args[i]));

    return out;
}

// --- the lint verb ------------------------------------------------------------------------------------

nx::args_builder build_lint_cli(options& opts, cc::vector<cc::string_view>& trailing)
{
    auto args = nx::args({
        .name = "shaped-linter",
        .description = "a self-contained C++ custom linter for shaped-core",
        .help = "Lints our own rules (default-init-assignment, qualified-primitive, no-flow-prose, "
                "no-long-prose-line) using a lexer and parser built here — no LLVM or libclang.",
    });

    args.positional("FILE", opts.files, {.desc = "source files to lint"});
    args.arg({"fix"}, opts.apply_fixes, "apply each finding's suggested edit back to its file in place");
    args.arg({"changed-lines"}, opts.changed_lines_path,
             {.desc = "report PROSE findings only on the lines named there (see readme)", .metavar = "FILE"});
    add_color_options(args, opts.color);

    // Everything after `--` is a file, even one starting with `-`.
    args.rest(trailing, "FILE", "more files, whatever they start with");

    args.section("verbs", "shaped-linter prose apply [options] <plan>\n"
                          "shaped-linter prose stats [options] <file>...\n"
                          "shaped-linter bless-includes [options] <file>...");

    finish(args);
    return args;
}

// --- prose apply --------------------------------------------------------------------------------------

nx::args_builder build_prose_apply_cli(prose_apply_options& opts)
{
    auto args = nx::args({
        .name = "shaped-linter prose apply",
        .description = "rewrite prose across many files in one pass",
    });

    args.positional("PLAN", opts.plan_path, {.desc = "the plan file to read", .required = true});
    args.arg({"dry-run"}, opts.dry_run, "validate the plan and report, but write nothing");
    args.arg({"stats"}, opts.stats, "also report the prose delta — lines and words, before and after, per file");
    add_color_options(args, opts.color);

    args.section("the plan format", R"(The plan names line spans and the prose to put there:

  ## libs/base/clean-core/src/clean-core/container/key_value_cache.hh
  [14-17]
  | /// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
  | /// The tier interface is the extension seam for on-disk / networked caches.
  [49-50]
  [+52]
  | /// Eviction is deliberately crude — see apply_bookkeeping.

`[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span with no `| ` lines deletes.
Spans ascend and may not overlap.
Everything after `| ` is verbatim, comment marker and indentation included — nothing is inferred, and EVERY replacement line needs its own `| ` prefix.)");

    args.section("what a run guarantees",
                 "Applying is all-or-nothing. A file is rejected if the edit changed code rather than prose, or if a "
                 "rule fires on a line the plan wrote. Every file is still judged after one fails, so a run reports "
                 "every problem the plan has rather than only the first.\n"
                 "The full grammar is in tools/shaped-linter/readme.md.");

    finish(args);
    return args;
}

// --- prose stats --------------------------------------------------------------------------------------

nx::args_builder build_prose_stats_cli(prose_stats_options& opts)
{
    auto args = nx::args({
        .name = "shaped-linter prose stats",
        .description = "how much prose a file carries",
    });

    args.positional("FILE", opts.files, {.desc = "files to measure"});
    add_color_options(args, opts.color);

    args.section("what it counts",
                 "Reports prose lines and words per file, then a total. Counts only extracted prose, so a `///` "
                 "marker, a `*` leader and the code around them never register; markdown keeps the `#` and `1.` "
                 "markers it reads as text.\n"
                 "The same measure `prose apply --stats` reports a delta in, so a rework can be scoped and budgeted "
                 "before its plan is written.");

    finish(args);
    return args;
}

// --- bless-includes -----------------------------------------------------------------------------------

nx::args_builder build_bless_includes_cli(bless_includes_options& opts)
{
    auto args = nx::args({
        .name = "shaped-linter bless-includes",
        .description = "the baseline every .shaped-lint.yml needs to be green",
    });

    args.positional("FILE", opts.files, {.desc = "source files to scan"});
    args.arg({"write"}, opts.write, "rewrite each config's generated block in place");
    args.arg({"files-from"}, opts.files_from_path,
             {.desc = "also scan the paths named there, one per line", .metavar = "FILE"});
    add_color_options(args, opts.color);

    args.section("what it writes",
                 "Scans the given C++ files for angle includes that are not ours and not yet blessed, and emits an "
                 "`allow-include` entry for each into the nearest .shaped-lint.yml above it. Without --write it "
                 "prints what each file would become.\n"
                 "The entries land between generated markers at the end of the config, so curated entries above them "
                 "survive a re-run and a re-run reproduces the block byte for byte. Curating means moving an entry out "
                 "of the block — or deleting it and fixing the include — which is exactly what the next diff shows.\n"
                 "A file with no .shaped-lint.yml above it is skipped: the tool fills configs in, it does not decide "
                 "where they belong.");

    finish(args);
    return args;
}

/// The help page for a verb, rendered from a throwaway options struct.
/// Colour is resolved by the time main prints this, so the global flag is the right one to read.
template <class Options, class Build>
cc::string render_usage(Build&& build)
{
    auto opts = Options();
    auto args = build(opts);

    return args.help_text({.color = cc::console::color_enabled(), .width = cc::console::terminal_width().value_or(100)});
}
} // namespace

cc::result<options, cc::string> parse_options(cc::span<char const* const> args)
{
    auto opts = options();
    auto trailing = cc::vector<cc::string_view>();
    auto parser = build_lint_cli(opts, trailing);

    auto const result = parser.parse(tokens_of(args, 1));
    if (auto const o = interpret(result, opts.help); o.handled)
        return o.failed ? cc::result<options, cc::string>(cc::error(o.message))
                        : cc::result<options, cc::string>(cc::move(opts));

    for (auto const& file : trailing)
        opts.files.push_back(cc::string(file));

    if (opts.files.empty())
        return cc::error("no input files (see --help)");

    return opts;
}

cc::result<prose_apply_options, cc::string> parse_prose_apply_options(cc::span<char const* const> args)
{
    auto opts = prose_apply_options();
    auto parser = build_prose_apply_cli(opts);

    auto const result = parser.parse(tokens_of(args, 0));
    if (auto const o = interpret(result, opts.help); o.handled)
        return o.failed ? cc::result<prose_apply_options, cc::string>(cc::error(o.message))
                        : cc::result<prose_apply_options, cc::string>(cc::move(opts));

    return opts;
}

cc::result<prose_stats_options, cc::string> parse_prose_stats_options(cc::span<char const* const> args)
{
    auto opts = prose_stats_options();
    auto parser = build_prose_stats_cli(opts);

    auto const result = parser.parse(tokens_of(args, 0));
    if (auto const o = interpret(result, opts.help); o.handled)
        return o.failed ? cc::result<prose_stats_options, cc::string>(cc::error(o.message))
                        : cc::result<prose_stats_options, cc::string>(cc::move(opts));

    if (opts.files.empty())
        return cc::error("no input files (see prose stats --help)");

    return opts;
}

cc::result<bless_includes_options, cc::string> parse_bless_includes_options(cc::span<char const* const> args)
{
    auto opts = bless_includes_options();
    auto parser = build_bless_includes_cli(opts);

    auto const result = parser.parse(tokens_of(args, 0));
    if (auto const o = interpret(result, opts.help); o.handled)
        return o.failed ? cc::result<bless_includes_options, cc::string>(cc::error(o.message))
                        : cc::result<bless_includes_options, cc::string>(cc::move(opts));

    if (opts.files.empty() && opts.files_from_path.empty())
        return cc::error("no input files (see bless-includes --help)");

    return opts;
}

cc::string_view usage_text()
{
    // Rendered once and kept, because the declared return type is a view.
    static auto const text = []
    {
        auto opts = options();
        auto trailing = cc::vector<cc::string_view>();
        auto args = build_lint_cli(opts, trailing);
        return args.help_text(
            {.color = cc::console::color_enabled(), .width = cc::console::terminal_width().value_or(100)});
    }();

    return text;
}

cc::string_view prose_apply_usage_text()
{
    static auto const text = render_usage<prose_apply_options>([](auto& o) { return build_prose_apply_cli(o); });
    return text;
}

cc::string_view prose_stats_usage_text()
{
    static auto const text = render_usage<prose_stats_options>([](auto& o) { return build_prose_stats_cli(o); });
    return text;
}

cc::string_view bless_includes_usage_text()
{
    static auto const text = render_usage<bless_includes_options>([](auto& o) { return build_bless_includes_cli(o); });
    return text;
}
} // namespace scl
