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

    /// --changed-lines <file>: a spec of the lines PROSE findings may be reported on (see changed_lines).
    /// Empty means report everywhere.
    /// Code findings ignore it entirely.
    cc::string changed_lines_path;

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

/// Parsed command line for `shaped-linter prose apply`.
struct prose_apply_options
{
    /// The plan file to read.
    cc::string plan_path;

    /// --dry-run: validate the whole plan and report, but write nothing.
    bool dry_run = false;

    /// --stats: also report each file's prose lines and words before and after, plus the total.
    bool stats = false;

    /// --color auto|always|never, as for the lint command.
    cc::console::color_mode color = cc::console::color_mode::automatic;

    /// -h / --help: main prints usage and exits 0.
    bool help = false;
};

/// Parse the arguments AFTER the `prose apply` verb — `args` holds nothing to skip.
/// Fails on an unknown flag, a missing plan path, or more than one.
cc::result<prose_apply_options> parse_prose_apply_options(cc::span<char const* const> args);

/// The `prose apply` usage text.
cc::string_view prose_apply_usage_text();
} // namespace scl
