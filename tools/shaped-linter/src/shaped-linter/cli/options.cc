#include "options.hh"

#include <clean-core/string/format.hh>

namespace scl
{
namespace
{
cc::result<cc::console::color_mode> parse_color_mode(cc::string_view text)
{
    if (text == "auto")
        return cc::console::color_mode::automatic;
    if (text == "always")
        return cc::console::color_mode::always;
    if (text == "never")
        return cc::console::color_mode::never;

    return cc::error(cc::format("unknown color mode '{}' (auto, always or never)", text));
}
} // namespace

cc::result<options> parse_options(cc::span<char const* const> args)
{
    options opts;
    bool positional_only = false; // set once we see `--`

    // argv[0] is the program itself.
    for (isize i = 1; i < args.size(); ++i)
    {
        cc::string_view const arg = args[i];

        if (positional_only)
        {
            opts.files.push_back(cc::string(arg));
            continue;
        }

        // Everything after `--` is a file, even if it starts with `-`.
        if (arg == "--")
        {
            positional_only = true;
            continue;
        }

        if (arg == "-h" || arg == "--help")
        {
            opts.help = true;
            return opts;
        }

        if (arg == "--fix")
        {
            opts.apply_fixes = true;
            continue;
        }

        if (arg == "--changed-lines")
        {
            if (i + 1 >= args.size())
                return cc::error("--changed-lines needs a file");

            ++i;
            opts.changed_lines_path = cc::string(cc::string_view(args[i]));
            continue;
        }

        if (arg == "--no-color")
        {
            opts.color = cc::console::color_mode::never;
            continue;
        }

        if (arg == "--color")
        {
            if (i + 1 >= args.size())
                return cc::error("--color needs a mode (auto, always or never)");

            ++i;
            auto mode = parse_color_mode(args[i]);
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("--color="))
        {
            auto mode = parse_color_mode(arg.subview(cc::string_view("--color=").size()));
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        // An unknown flag is a hard error; a bare token is a file to lint.
        if (arg.starts_with("-") && arg != "-")
            return cc::error(cc::format("unknown argument '{}' (see --help)", arg));

        opts.files.push_back(cc::string(arg));
    }

    if (!opts.help && opts.files.empty())
        return cc::error("no input files (see --help)");

    return opts;
}

cc::result<prose_apply_options> parse_prose_apply_options(cc::span<char const* const> args)
{
    prose_apply_options opts;
    auto have_plan = false;

    for (isize i = 0; i < args.size(); ++i)
    {
        cc::string_view const arg = args[i];

        if (arg == "-h" || arg == "--help")
        {
            opts.help = true;
            return opts;
        }

        if (arg == "--dry-run")
        {
            opts.dry_run = true;
            continue;
        }

        if (arg == "--stats")
        {
            opts.stats = true;
            continue;
        }

        if (arg == "--no-color")
        {
            opts.color = cc::console::color_mode::never;
            continue;
        }

        if (arg == "--color")
        {
            if (i + 1 >= args.size())
                return cc::error("--color needs a mode (auto, always or never)");

            ++i;
            auto mode = parse_color_mode(args[i]);
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("--color="))
        {
            auto mode = parse_color_mode(arg.subview(cc::string_view("--color=").size()));
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("-") && arg != "-")
            return cc::error(cc::format("unknown argument '{}' (see prose apply --help)", arg));

        if (have_plan)
            return cc::error("prose apply takes exactly one plan file");

        opts.plan_path = cc::string(arg);
        have_plan = true;
    }

    if (!opts.help && !have_plan)
        return cc::error("no plan file (see prose apply --help)");

    return opts;
}

cc::result<prose_stats_options> parse_prose_stats_options(cc::span<char const* const> args)
{
    prose_stats_options opts;

    for (isize i = 0; i < args.size(); ++i)
    {
        cc::string_view const arg = args[i];

        if (arg == "-h" || arg == "--help")
        {
            opts.help = true;
            return opts;
        }

        if (arg == "--no-color")
        {
            opts.color = cc::console::color_mode::never;
            continue;
        }

        if (arg == "--color")
        {
            if (i + 1 >= args.size())
                return cc::error("--color needs a mode (auto, always or never)");

            ++i;
            auto mode = parse_color_mode(args[i]);
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("--color="))
        {
            auto mode = parse_color_mode(arg.subview(cc::string_view("--color=").size()));
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("-") && arg != "-")
            return cc::error(cc::format("unknown argument '{}' (see prose stats --help)", arg));

        opts.files.push_back(cc::string(arg));
    }

    if (!opts.help && opts.files.empty())
        return cc::error("no input files (see prose stats --help)");

    return opts;
}

cc::result<bless_includes_options> parse_bless_includes_options(cc::span<char const* const> args)
{
    bless_includes_options opts;

    for (isize i = 0; i < args.size(); ++i)
    {
        cc::string_view const arg = args[i];

        if (arg == "-h" || arg == "--help")
        {
            opts.help = true;
            return opts;
        }

        if (arg == "--write")
        {
            opts.write = true;
            continue;
        }

        if (arg == "--files-from")
        {
            if (i + 1 >= args.size())
                return cc::error("--files-from needs a file");

            ++i;
            opts.files_from_path = cc::string(cc::string_view(args[i]));
            continue;
        }

        if (arg == "--no-color")
        {
            opts.color = cc::console::color_mode::never;
            continue;
        }

        if (arg == "--color")
        {
            if (i + 1 >= args.size())
                return cc::error("--color needs a mode (auto, always or never)");

            ++i;
            auto mode = parse_color_mode(args[i]);
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("--color="))
        {
            auto mode = parse_color_mode(arg.subview(cc::string_view("--color=").size()));
            CC_RETURN_IF_ERROR(mode);

            opts.color = mode.value();
            continue;
        }

        if (arg.starts_with("-") && arg != "-")
            return cc::error(cc::format("unknown argument '{}' (see bless-includes --help)", arg));

        opts.files.push_back(cc::string(arg));
    }

    if (!opts.help && opts.files.empty() && opts.files_from_path.empty())
        return cc::error("no input files (see bless-includes --help)");

    return opts;
}

cc::string_view usage_text()
{
    return R"(shaped-linter — a self-contained C++ custom linter for shaped-core

usage:
  shaped-linter [options] <file>... [-- <file>...]
  shaped-linter prose apply [options] <plan>
  shaped-linter prose stats [options] <file>...
  shaped-linter bless-includes [options] <file>...

options:
  --fix                    apply each finding's suggested edit back to its file in place
  --changed-lines <file>   report PROSE findings only on the lines named there (see readme)
  --color <mode>           auto (default), always or never; auto colors only on a terminal
  --no-color               the old spelling of --color never
  -h / --help              print this and exit

Everything after a `--` is taken as a file, even when it starts with a `-`.

Lints our own rules (default-init-assignment, qualified-primitive, no-flow-prose,
no-long-prose-line) using a lexer and parser built here — no LLVM or libclang.
)";
}

cc::string_view prose_apply_usage_text()
{
    return R"(shaped-linter prose apply — rewrite prose across many files in one pass

usage:
  shaped-linter prose apply [options] <plan>

options:
  --dry-run        validate the plan and report, but write nothing
  --stats          also report the prose delta — lines and words, before and after, per file
  --color <mode>   auto (default), always or never
  --no-color       the old spelling of --color never
  -h / --help      print this and exit

The plan names line spans and the prose to put there:

  ## libs/base/clean-core/src/clean-core/container/key_value_cache.hh
  [14-17]
  | /// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
  | /// The tier interface is the extension seam for on-disk / networked caches.
  [49-50]
  [+52]
  | /// Eviction is deliberately crude — see apply_bookkeeping.

`[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span
with no `| ` lines deletes. Spans ascend and may not overlap. Everything after `| ` is
verbatim, comment marker and indentation included — nothing is inferred, and EVERY
replacement line needs its own `| ` prefix.

Applying is all-or-nothing. A file is rejected if the edit changed code rather than
prose, or if a rule fires on a line the plan wrote. Every file is still judged after
one fails, so a run reports every problem the plan has rather than only the first.

The full grammar is in tools/shaped-linter/readme.md.
)";
}

cc::string_view prose_stats_usage_text()
{
    return R"(shaped-linter prose stats — how much prose a file carries

usage:
  shaped-linter prose stats [options] <file>...

options:
  --color <mode>   auto (default), always or never
  --no-color       the old spelling of --color never
  -h / --help      print this and exit

Reports prose lines and words per file, then a total. Counts only extracted prose,
so a `///` marker, a `*` leader and the code around them never register; markdown
keeps the `#` and `1.` markers it reads as text.

The same measure `prose apply --stats` reports a delta in, so a rework can be scoped
and budgeted before its plan is written.
)";
}

cc::string_view bless_includes_usage_text()
{
    return R"(shaped-linter bless-includes — the baseline every .shaped-lint.yml needs to be green

usage:
  shaped-linter bless-includes [options] <file>...

options:
  --write          rewrite each config's generated block in place
  --files-from <f> also scan the paths named there, one per line
  --color <mode>   auto (default), always or never
  --no-color       the old spelling of --color never
  -h / --help      print this and exit

Scans the given C++ files for angle includes that are not ours and not yet blessed,
and emits an `allow-include` entry for each into the nearest .shaped-lint.yml above
it. Without --write it prints what each file would become.

The entries land between generated markers at the end of the config, so curated
entries above them survive a re-run and a re-run reproduces the block byte for byte.
Curating means moving an entry out of the block — or deleting it and fixing the
include — which is exactly what the next diff then shows.

A file with no .shaped-lint.yml above it is skipped: the tool fills configs in, it
does not decide where they belong.
)";
}
} // namespace scl
