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

cc::string_view usage_text()
{
    return R"(shaped-linter — a self-contained C++ custom linter for shaped-core

usage:
  shaped-linter [options] <file>... [-- <file>...]

options:
  --fix            apply each finding's suggested edit back to its file in place
  --color <mode>   auto (default), always or never; auto colors only on a terminal
  --no-color       the old spelling of --color never
  -h / --help      print this and exit

Lints its own rules (starting with member-default-init-assignment) on shaped-core's
libraries, using a lexer and parser built here — no LLVM or libclang.
)";
}
} // namespace scl
