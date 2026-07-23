#include "options.hh"

#include <clean-core/string/format.hh>

namespace scl
{
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

        if (arg == "--no-color")
        {
            opts.no_color = true;
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

cc::string_view usage_text()
{
    return R"(shaped-linter — a self-contained C++ custom linter for shaped-core

usage:
  shaped-linter [options] <file>... [-- <file>...]

options:
  --fix            apply each finding's suggested edit back to its file in place
  --no-color       force plain output even on a terminal
  -h / --help      print this and exit

Lints its own rules (starting with member-default-init-assignment) on shaped-core's
libraries, using a lexer and parser built here — no LLVM or libclang.
)";
}
} // namespace scl
