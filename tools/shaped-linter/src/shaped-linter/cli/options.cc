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

cc::string_view usage_text()
{
    return R"(shaped-linter — a self-contained C++ custom linter for shaped-core

usage:
  shaped-linter [options] <file>... [-- <file>...]
  shaped-linter prose apply [options] <plan>

options:
  --fix                    apply each finding's suggested edit back to its file in place
  --changed-lines <file>   report PROSE findings only on the lines named there (see readme)
  --color <mode>           auto (default), always or never; auto colors only on a terminal
  --no-color               the old spelling of --color never
  -h / --help              print this and exit

Lints its own rules (starting with member-default-init-assignment) on shaped-core's
libraries, using a lexer and parser built here — no LLVM or libclang.
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
  | /// A thread-safe, tiered get-or-create cache.
  | /// Providers are queried front-to-back, fastest first.
  [49-50]
  [+52]
  | /// Eviction is deliberately crude — see apply_bookkeeping.

`[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span
with no `| ` lines deletes. Spans ascend and may not overlap. Everything after `| ` is
verbatim, comment marker and indentation included — nothing is inferred.

Applying is all-or-nothing. A file is rejected if the edit changed code rather than
prose, or if a rule fires on a line the plan wrote.
)";
}
} // namespace scl
