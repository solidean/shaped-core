#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>

// =========================================================================================================
// What a parse concluded, and everything it found wrong on the way.
//
// Diagnostics are STRUCTURED and rendered separately, which is what lets a test assert on the fact rather
// than on the wording, and lets one message change in one place.
//
// A parse accumulates: every unknown option, every missing required argument and every failed conversion
// come back from ONE run, so a user fixes their command line once instead of five times.
// =========================================================================================================

/// Which way a command line was wrong.
/// `setup_error` is the odd one out: it means the PROGRAM declared its arguments wrongly, so it is never
/// the user's fault, carries no suggestion, and prints no usage line.
enum class nx::diagnostic_kind
{
    unknown_option,
    unknown_command,
    missing_value,    // the option is last on the line and takes a value
    unexpected_value, // `--flag=x` where the flag takes none
    invalid_value,    // the type refused the token
    missing_required,
    unexpected_positional, // more positionals than were declared
    missing_positional,    // fewer than the declared minimum
    unexpected_separator,  // a bare `--` with nothing declared to receive the tail
    failed_validation,
    setup_error,
};

/// Where a token came from, since splicing means an index into argv is not always the truth.
enum class nx::arg_origin
{
    command_line,
    response_file,
    environment,
    test_args,
};

struct nx::arg_source
{
    arg_origin origin = arg_origin::command_line;
    cc::string name;  // the response file's path, or the environment variable's name
    isize index = -1; // which token within that origin, or -1 when the diagnostic is about no token at all
};

/// One thing wrong, as data.
/// `message` is the whole sentence a user reads; the other fields are what a test asserts on.
struct nx::args_diagnostic
{
    diagnostic_kind kind = diagnostic_kind::unknown_option;
    arg_source source;
    cc::string token;      // the offending text, empty when the problem is an absence
    cc::string arg_name;   // the canonical name of the argument involved, when there is one
    cc::string message;    // what went wrong, without the "error: " prefix
    cc::string suggestion; // the one thing they probably meant, empty when nothing is close enough
};

/// What the outcome of a parse was, beyond pass or fail.
enum class nx::args_outcome
{
    success,
    help_requested,
    version_requested,
    completion_requested,
    usage_error,
    setup_error,
};

/// How diagnostics and help are turned into text.
/// Both are explicit rather than sniffed from the process, which is what makes rendered output testable.
struct nx::args_render_options
{
    bool color = false;
    int width = 100;
};

/// What a parse concluded, and what the process should do about it.
///
/// The whole point of the three-state shape is that `--help` is neither success nor failure: it means the
/// program did what was asked and should exit zero, which a bool return cannot express.
///
///     if (auto const r = args.parse(argc, argv); r.should_exit())
///         return r.exit_code();
class nx::args_result
{
public:
    args_result() = default;
    args_result(args_outcome outcome, int exit_code) : _outcome(outcome), _exit_code(exit_code) {}

    /// The command line parsed and every bound variable is usable.
    [[nodiscard]] bool ok() const { return _outcome == args_outcome::success; }

    /// The program should stop and return `exit_code()` — whether because it failed or because it already
    /// did what was asked, as --help and --version do.
    [[nodiscard]] bool should_exit() const { return _outcome != args_outcome::success; }

    [[nodiscard]] args_outcome outcome() const { return _outcome; }
    [[nodiscard]] int exit_code() const { return _exit_code; }

    [[nodiscard]] cc::span<args_diagnostic const> diagnostics() const { return _diagnostics; }
    [[nodiscard]] bool has_diagnostics() const { return !_diagnostics.empty(); }

    /// The rendered form of every diagnostic, as it would be printed.
    [[nodiscard]] cc::string diagnostic_text(args_render_options const& options = {}) const;

    void add_diagnostic(args_diagnostic diagnostic) { _diagnostics.push_back(cc::move(diagnostic)); }
    void set_outcome(args_outcome outcome, int exit_code)
    {
        _outcome = outcome;
        _exit_code = exit_code;
    }

private:
    args_outcome _outcome = args_outcome::success;
    int _exit_code = 0;
    cc::vector<args_diagnostic> _diagnostics;
};

namespace nx
{

/// The exit code a declaration bug produces, distinct from any usage error so a script can tell them apart.
/// 70 is sysexits' EX_SOFTWARE: an internal error, not a bad invocation.
inline constexpr int args_setup_exit_code = 70;

} // namespace nx

namespace nx::impl
{
/// One diagnostic as a line (or three, when it carries a suggestion), without the trailing newline.
cc::string render_diagnostic(args_diagnostic const& diagnostic, args_render_options const& options);

/// Every diagnostic, plus the "try 'app --help'" footer when `usage_hint` is non-empty.
cc::string render_diagnostics(cc::span<args_diagnostic const> diagnostics,
                              cc::string_view usage_hint,
                              args_render_options const& options);

} // namespace nx::impl
