#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
/// Parsed command line for the shaped-linter executable.
/// See usage_text() for the flags and readme.md for what they mean.
struct options
{
    /// Source files to lint. At least one is required (unless --help).
    cc::vector<cc::string> files;

    /// --fix: apply each finding's suggested edit back to its file in place.
    bool apply_fixes = false;

    /// --color auto|always|never (`--no-color` is the old spelling of `never`).
    /// `auto` colors only when both stdout and stderr are terminals — see cc::console.
    cc::console::color_mode color = cc::console::color_mode::automatic;

    /// -h / --help: main prints usage and exits 0.
    bool help = false;
};

/// Parse argv (including argv[0], which is ignored).
/// `--` stops flag parsing; every later argument is a file, even one starting with `-`.
/// Fails on an unknown `--flag` or, after parsing, on no input files.
cc::result<options> parse_options(cc::span<char const* const> args);

/// The --help / usage text.
cc::string_view usage_text();
} // namespace scl
