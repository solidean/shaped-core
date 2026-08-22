#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/diagnostic.hh>
#include <nexus/args/impl/binding.hh>
#include <nexus/args/impl/command.hh>
#include <nexus/args/options.hh>

/// One command line, declared.
///
/// Bind local variables, parse, and use them — the variables' own initializers ARE the defaults, so a
/// default cannot disagree with itself and help prints what the program will actually do.
///
///     auto jobs = 4;
///     auto verbose = false;
///
///     auto args = nx::args({.name = "mytool", .description = "does the thing", .version = "0.1"});
///     args.arg({"j", "jobs"}, jobs, {.desc = "how many jobs to run at once"});
///     args.arg({"v", "verbose"}, verbose, "print more");
///
///     if (auto const r = args.parse(argc, argv); r.should_exit())
///         return r.exit_code();
///
/// The builder is move-only and its methods mutate rather than chain off a temporary, so name it.
/// `group` is a mode setter: every argument declared after it joins that section of the help.
///
/// libs/base/nexus/docs/args.md is the full reference, and carries the token grammar as a spec.
class nx::args_builder
{
public:
    explicit args_builder(app_info info);
    ~args_builder();

    args_builder(args_builder&&) noexcept;
    args_builder& operator=(args_builder&&) noexcept;
    args_builder(args_builder const&) = delete;
    args_builder& operator=(args_builder const&) = delete;

    // =====================================================================================================
    // Declaring arguments
    //
    // Names are always braced: {"j", "jobs"}.
    // A single character is short, longer is long, and a spelling carrying its own dashes is taken verbatim.
    // The first entry is canonical.
    // =====================================================================================================

    /// One named argument writing one variable.
    template <arg_value T>
    args_builder& arg(cc::span<arg::name_spec const> names, T& target, arg_options<T> opts = {})
    {
        auto b = impl::binding();
        impl::bind_scalar(b, target, !opts.required);
        impl::bind_make_default(b, target, opts);
        impl::bind_validator(b, target, opts);
        b.takes_value = !b.is_bool;
        configure(b, names, impl::to_common(opts));
        return add_option(cc::move(b));
    }

    /// The same, with only a description — the common case, without the braces.
    template <arg_value T>
    args_builder& arg(cc::span<arg::name_spec const> names, T& target, cc::string_view desc)
    {
        return arg(names, target, arg_options<T>{.desc = desc});
    }

    /// A repeatable argument: every occurrence appends rather than replacing.
    template <arg_value T>
    args_builder& arg(cc::span<arg::name_spec const> names, cc::vector<T>& target, arg_options<T> opts = {})
    {
        auto b = impl::binding();
        impl::bind_vector(b, target);
        impl::bind_element_validator(b, opts);
        b.takes_value = true;
        configure(b, names, impl::to_common(opts));
        return add_option(cc::move(b));
    }

    template <arg_value T>
    args_builder& arg(cc::span<arg::name_spec const> names, cc::vector<T>& target, cc::string_view desc)
    {
        return arg(names, target, arg_options<T>{.desc = desc});
    }

    /// A flag that counts its occurrences, so `-vvv` leaves the target at 3.
    /// Takes no value in any form, which is what makes clustering it unambiguous.
    template <class T>
    args_builder& count(cc::span<arg::name_spec const> names, T& target, arg_options<T> opts = {})
    {
        auto b = impl::binding();
        impl::bind_counter(b, target);
        b.takes_value = false;
        configure(b, names, impl::to_common(opts));
        return add_option(cc::move(b));
    }

    template <class T>
    args_builder& count(cc::span<arg::name_spec const> names, T& target, cc::string_view desc)
    {
        return count(names, target, arg_options<T>{.desc = desc});
    }

    /// A flag that runs something instead of writing a variable.
    /// Actions fire in token order, during the parse — so a line that later fails may already have run some.
    args_builder& action(cc::span<arg::name_spec const> names,
                         cc::unique_function<void()> fn,
                         arg_options<bool> opts = {});
    args_builder& action(cc::span<arg::name_spec const> names, cc::unique_function<void()> fn, cc::string_view desc);

    /// The same, taking a value — the `-I path` shape, where order matters and a vector would lose it.
    args_builder& value_action(cc::span<arg::name_spec const> names,
                               cc::unique_function<void(cc::string_view)> fn,
                               arg_options<cc::string> opts = {});

    /// One argument taken by position rather than by name.
    template <arg_value T>
    args_builder& positional(cc::string_view metavar, T& target, arg_options<T> opts = {})
    {
        auto b = impl::binding();
        b.kind = impl::binding_kind::positional;
        impl::bind_scalar(b, target, !opts.required);
        impl::bind_make_default(b, target, opts);
        impl::bind_validator(b, target, opts);
        b.takes_value = true;
        configure_positional(b, metavar, impl::to_common(opts));
        return add_positional(cc::move(b));
    }

    /// The variadic positional — at most one, and it may sit anywhere among the fixed ones.
    /// `min_count` and `max_count` on the options bound its arity and are printed in the usage line.
    template <arg_value T>
    args_builder& positional(cc::string_view metavar, cc::vector<T>& target, arg_options<T> opts = {})
    {
        auto b = impl::binding();
        b.kind = impl::binding_kind::positional;
        impl::bind_vector(b, target);
        impl::bind_element_validator(b, opts);
        b.takes_value = true;
        configure_positional(b, metavar, impl::to_common(opts));
        return add_positional(cc::move(b));
    }

    // =====================================================================================================
    // Subcommands
    //
    // A subcommand is a nested builder, so nesting costs nothing and help at any depth is the same code.
    // `declare` runs LAZILY — only when the command is selected, or when help or completion needs it — so
    // it must be pure declaration, callable at any time before the parse returns, and idempotent.
    // =====================================================================================================

    /// One subcommand, whose arguments `declare` will add to its own nested builder.
    args_builder& command(cc::span<arg::name_spec const> names,
                          cc::string_view desc,
                          cc::unique_function<void(args_builder&)> declare);

    /// A command this program does not own: everything after its name goes to `run` untouched, `--help`
    /// and `--` included, and whatever it returns becomes the exit code.
    /// Help lists it without pretending to know its options, because it genuinely cannot.
    args_builder& delegate(cc::span<arg::name_spec const> names,
                           cc::string_view desc,
                           cc::unique_function<int(cc::span<cc::string_view const>)> run);

    /// Accept this option at any depth, rather than only before the command name.
    /// Declared rather than emergent, because "which options survive the subcommand" is exactly what rots
    /// in a hand-rolled parser.
    args_builder& global();

    /// Run this command when none is named, instead of failing.
    args_builder& default_command(cc::string_view name);

    /// Turn off the implicit `help <command>` subcommand.
    args_builder& no_auto_help_command();

    /// Which command ran, as its canonical name — empty when none did.
    [[nodiscard]] cc::string_view selected_command() const;

    /// The whole chain for a nested command, outermost first.
    [[nodiscard]] cc::span<cc::string const> command_path() const { return _command_path; }

    /// Whether the chain is exactly `path`, e.g. "remote add".
    [[nodiscard]] bool is_command(cc::string_view path) const;

    /// Everything after a bare `--`.
    /// Declaring this is what makes `--` legal: without it a `--` on the command line is an error rather
    /// than a silent way to drop the rest of what the user typed.
    args_builder& rest(cc::vector<cc::string_view>& target, cc::string_view metavar = "ARGS", cc::string_view desc = {});

    // =====================================================================================================
    // Shaping the help
    // =====================================================================================================

    /// Replace the name, description and version.
    /// What a `declare_args` function calls first, since parse_args cannot know them before it asks.
    args_builder& info(app_info value);

    /// Every argument declared after this joins `title`. A mode setter, not a per-argument property.
    args_builder& group(cc::string_view title);

    /// Free-form prose in the help, after the argument table.
    args_builder& section(cc::string_view title, cc::string_view text);

    /// One worked invocation, shown verbatim under EXAMPLES.
    args_builder& example(cc::string_view command_line, cc::string_view desc = {});

    /// An environment variable the PROGRAM reads for itself — documentation only, with no parse effect.
    /// The `env` field on an argument is the other one: that one is actually read as a fallback value.
    args_builder& document_env(cc::string_view name, cc::string_view desc);

    // =====================================================================================================
    // Behaviour
    // =====================================================================================================

    // =====================================================================================================
    // Cross-argument rules
    //
    // Each states itself in the CONSTRAINTS section of the help, so a rule cannot be enforced silently.
    // All of them are skipped when anything earlier already failed: a rule read over a half-bound command
    // line produces nonsense, and nonsense on top of a real error is worse than saying nothing.
    // =====================================================================================================

    /// A rule of your own over the bound variables.
    args_builder& require(cc::string_view description, cc::unique_function<bool()> predicate);

    /// Giving `name` means every one of `required` must be given too.
    args_builder& requires_all(cc::string_view name, cc::span<cc::string_view const> required);

    /// At most one of these may be given.
    args_builder& mutually_exclusive(cc::span<cc::string_view const> names);

    /// At least one of these must be given.
    args_builder& at_least_one_of(cc::span<cc::string_view const> names);

    /// Collect unrecognized tokens instead of failing on them — for a wrapper that forwards them onward.
    args_builder& allow_unknown(cc::vector<cc::string_view>& target);

    /// Expand `@file` tokens into the tokens that file contains, before parsing.
    ///
    /// Opt-in rather than automatic: a program that takes user-supplied filenames would otherwise gain a
    /// file-read primitive nobody asked it for.
    /// `@@rest` is a literal `@rest`, and expansion stops at a bare `--`.
    args_builder& enable_response_files(int max_depth = 8);

    /// Treat the first positional as the end of options, leaving the tail untouched.
    args_builder& stop_at_first_positional();

    /// Do not read `--help` / `-h` or `--version` unless they were declared explicitly.
    args_builder& no_auto_help();
    args_builder& no_auto_version();

    /// Do not answer `--completion <shell>` with a generated completion script.
    args_builder& no_auto_completion();

    /// Do not print help or diagnostics from `parse` — the caller renders them itself.
    /// What a test wants, so that asserting on output does not mean capturing stdout.
    args_builder& no_auto_print();

    /// Print the whole help after a usage error, rather than the error plus a one-line hint.
    args_builder& full_help_on_error();

    /// Stop treating `_` as `-` when looking up a long name.
    args_builder& exact_long_names();

    /// What `parse` returns for a bad command line.
    /// Help and version always exit zero.
    args_builder& usage_exit_code(int code);

    // =====================================================================================================
    // Parsing
    // =====================================================================================================

    /// argv[0] is the program path and is skipped.
    args_result parse(int argc, char const* const* argv);

    /// Every element is an argument: unlike the argv form, nothing is skipped.
    args_result parse(cc::span<cc::string_view const> tokens);

    /// Check the DECLARATION for contradictions — duplicate names, two variadic positionals, and so on.
    /// Runs inside every parse as well, in every preset, because a shipped binary must catch it too.
    /// Worth calling directly from a program's own test suite, which is the place it should be caught.
    [[nodiscard]] args_result validate_setup() const;

    // =====================================================================================================
    // Results and rendering
    // =====================================================================================================

    /// The full help page, as --help prints it.
    [[nodiscard]] cc::string help_text(args_render_options const& options = {}) const;

    /// The one-screen form, as -h prints it: no long descriptions, no sections, no examples.
    [[nodiscard]] cc::string short_help_text(args_render_options const& options = {}) const;

    /// Just the `usage: ...` line.
    [[nodiscard]] cc::string usage_line() const;

    void print_help() const;
    void print_short_help() const;

    /// Whether `name` appeared on the command line at least once, spelled as help spells it (`--jobs`).
    ///
    /// For the question a bound variable cannot answer: was this given, or is it merely at its default?
    /// A program that adjusts one setting only when the user left another alone needs exactly this, and
    /// comparing against the default is the wrong way to ask — the user may well have typed the default.
    [[nodiscard]] bool was_given(cc::string_view name) const;

    /// Everything after the bare `--`, whether or not a `rest` binding was declared.
    [[nodiscard]] cc::span<cc::string_view const> raw() const { return _raw; }

    [[nodiscard]] app_info const& info() const { return _info; }

private:
    args_builder& add_option(impl::binding b);
    args_builder& add_positional(impl::binding b);
    void configure(impl::binding& b, cc::span<arg::name_spec const> names, impl::common_options opts);
    void configure_positional(impl::binding& b, cc::string_view metavar, impl::common_options opts);

    struct section_entry
    {
        cc::string title;
        cc::string text;
    };
    struct example_entry
    {
        cc::string command_line;
        cc::string desc;
    };
    struct env_entry
    {
        cc::string name;
        cc::string desc;
    };

    app_info _info;
    cc::string _current_group;

    cc::vector<impl::binding> _bindings;
    cc::vector<isize> _positional_order; // indices into _bindings, in declaration order

    /// Set when this builder is a subcommand's, so a `global()` option declared above is still reachable.
    /// A pointer rather than copied bindings, because a binding owns move-only thunks and, more to the
    /// point, must keep writing the SAME variable the parent bound.
    args_builder* _parent = nullptr;

    cc::vector<impl::command_node> _commands;
    cc::vector<cc::unique_ptr<args_builder>> _subtrees; // owned by the ROOT, indexed by command_node::subtree
    cc::vector<cc::string> _command_path;
    cc::string _default_command;
    bool _auto_help_command = true;

    cc::vector<document_validator> _document_validators;
    cc::vector<section_entry> _sections;
    cc::vector<example_entry> _examples;
    cc::vector<env_entry> _documented_env;

    // Every parsed token, owned, so a bound string_view never dangles.
    // A vector<char> rather than strings because cc::string does small-string optimization: a view into a
    // short one would point inside the string object and break the moment the builder is moved.
    cc::vector<char> _token_storage;
    cc::vector<cc::string_view> _tokens;
    cc::vector<cc::string_view> _raw;
    cc::vector<cc::string_view>* _unknown_target = nullptr;

    bool _stop_at_first_positional = false;
    bool _response_files = false;
    int _response_file_depth = 8;
    bool _auto_help = true;
    bool _auto_version = true;
    bool _auto_completion = true;
    bool _auto_print = true;
    bool _full_help_on_error = false;
    bool _normalize_underscores = true;
    int _usage_exit_code = 1;

    // Set by the parse when the user asked for help rather than mistyped something.
    bool _help_requested = false;
    bool _short_help_requested = false;
    bool _version_requested = false;

    friend struct impl::parse_engine;
    friend struct impl::help_renderer;
    friend struct impl::setup_checker;
    friend struct impl::describer;
};

namespace nx
{

/// The named factory.
/// Takes only what a help page needs; the tokens come to `parse`.
[[nodiscard]] args_builder args(app_info info);

} // namespace nx
