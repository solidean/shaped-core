#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/fwd.hh>
#include <nexus/args/validation.hh>

/// What a shell should offer when completing an argument's value.
enum class nx::complete_hint
{
    automatic, // values() when the type publishes them, otherwise nothing
    none,      // offer nothing, even for a type that publishes values
    files,
    directories,
};

/// The name, description and version every help page is built around.
/// The views must outlive the builder, which string literals do by construction.
struct nx::app_info
{
    cc::string_view name;
    cc::string_view description; // one sentence, shown at the top of both help forms
    cc::string_view help;        // the longer text, shown only by --help
    cc::string_view version;     // enables --version when set
};

/// One declared name.
/// A single character is a short name (`-f`), anything longer is a long name (`--force`), and a spelling
/// that already carries its dashes is taken verbatim — so `{"x", "--x"}` declares both `-x` and `--x`.
struct nx::arg::name_spec
{
    cc::string_view text;
    bool hidden = false;

    constexpr name_spec(char const* t) : text(t) {}
    constexpr name_spec(cc::string_view t) : text(t) {}
    constexpr name_spec(cc::string_view t, bool h) : text(t), hidden(h) {}
};

namespace nx::arg
{

/// An alias that still parses and is still suggested on a typo, but never appears in help.
/// The spelling to use for a deprecated name kept alive for compatibility.
[[nodiscard]] constexpr name_spec hidden(cc::string_view name)
{
    return name_spec(name, true);
}

} // namespace nx::arg

/// Everything about one argument except its names and the variable it writes.
///
/// Written as a designated initializer at the call site — `{.desc = "how many jobs", .required = true}` —
/// so an argument reads as one declaration rather than a run of setters.
///
/// The default value is NOT here: it is whatever the bound variable already holds when the argument is
/// declared, which is the one place it cannot disagree with itself.
/// `make_default` covers the case where computing it is the point, and `default_text` overrides only how
/// the help spells it.
template <class T>
struct nx::arg_options
{
    /// One sentence, shown by both -h and --help.
    /// Ends without a period, like every other entry in the table.
    cc::string_view desc;

    /// The longer explanation, shown only by --help.
    /// May span paragraphs.
    cc::string_view help;

    /// The value placeholder in help and usage, e.g. "FILE".
    /// Defaults to the type's own name.
    cc::string_view metavar;

    /// Which help section this argument joins, overriding the builder's current group.
    cc::string_view group;

    /// An environment variable read when the argument is absent from the command line.
    /// Precedence is command line, then environment, then default.
    cc::string_view env;

    /// Overrides how the default is printed, for a default that is computed or not worth spelling out.
    cc::string_view default_text;

    /// When set, using the argument warns on stderr with this text, and it still parses.
    cc::string_view deprecated;

    /// Absent from the command line is an error.
    /// Suppresses default printing, so it is safe over an uninitialized variable — which is the only case
    /// where reading one before the parse would be wrong.
    bool required = false;

    /// Also accept `--no-<name>`, setting the flag false.
    /// Only meaningful for a bool.
    bool negatable = false;

    /// Parses and is suggested on a typo, but never appears in help.
    bool hidden = false;

    complete_hint complete = complete_hint::automatic;

    /// How many values a VARIADIC positional must and may take, ignored everywhere else.
    /// `max_count < 0` is unbounded.
    /// Both are printed in the usage line, so FILES... and [FILES...] differ.
    isize min_count = 0;
    isize max_count = -1;

    /// What the value must satisfy beyond parsing, e.g. `nx::arg::in_range(1, 256)`.
    /// The rule prints itself in help, and compose several with `&&`.
    arg_validator<T> validate;

    /// Computes the default, lazily and only when the argument is absent from a successful parse.
    /// Never called to render help — pair it with `default_text` for that.
    cc::unique_function<T()> make_default;
};
