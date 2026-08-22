#include "setup_check.hh"

#include <clean-core/string/format.hh>
#include <nexus/args/builder.hh>

namespace
{
using nx::isize;

nx::args_diagnostic setup_error(cc::string message, cc::string arg_name = {})
{
    return {
        .kind = nx::diagnostic_kind::setup_error,
        .arg_name = cc::move(arg_name),
        .message = cc::move(message),
    };
}
} // namespace

nx::args_result nx::impl::setup_checker::run(args_builder const& builder)
{
    auto result = args_result();

    auto variadic_positionals = isize(0);
    auto rest_bindings = isize(0);

    for (auto i = isize(0); i < builder._bindings.size(); ++i)
    {
        auto const& b = builder._bindings[i];

        if (b.kind == binding_kind::rest)
        {
            ++rest_bindings;
            continue;
        }

        if (b.kind == binding_kind::positional)
        {
            if (b.accumulates)
                ++variadic_positionals;

            if (b.metavar.empty())
                result.add_diagnostic(setup_error("a positional argument needs a metavar to name it in help and "
                                                  "errors"));

            continue;
        }

        // From here on: a named option.
        if (b.names.empty())
            result.add_diagnostic(setup_error("an option was declared with no names"));

        if (b.negatable && !b.is_bool)
            result.add_diagnostic(
                setup_error(cc::format("'{}' is negatable but does not bind a bool", b.canonical), b.canonical));

        if (b.required && b.apply_make_default.is_valid())
            result.add_diagnostic(setup_error(
                cc::format("'{}' is both required and has a make_default, so the default could never apply", b.canonical),
                b.canonical));

        if (b.counting && b.takes_value)
            result.add_diagnostic(setup_error(
                cc::format("'{}' counts occurrences and cannot also take a value", b.canonical), b.canonical));

        for (auto const& name : b.names)
        {
            if (name.text.empty())
            {
                result.add_diagnostic(setup_error(cc::format("'{}' has an empty name", b.canonical), b.canonical));
                continue;
            }

            if (name.is_short && name.text.size() != 1)
                result.add_diagnostic(setup_error(
                    cc::format("'{}' is declared short but is more than one character", name.display()), b.canonical));
        }

        // Names must be unique across every option, which is the failure this whole pass exists for.
        for (auto j = i + 1; j < builder._bindings.size(); ++j)
        {
            auto const& other = builder._bindings[j];
            if (other.kind != binding_kind::option)
                continue;

            for (auto const& name : b.names)
                for (auto const& other_name : other.names)
                    if (name.is_short == other_name.is_short && name.text == other_name.text)
                        result.add_diagnostic(setup_error(
                            cc::format("'{}' and '{}' both claim {}", b.canonical, other.canonical, name.display()),
                            b.canonical));
        }
    }

    if (variadic_positionals > 1)
        result.add_diagnostic(setup_error("more than one variadic positional was declared, so how to split the values "
                                          "between them is undefined"));

    if (rest_bindings > 1)
        result.add_diagnostic(setup_error("more than one rest binding was declared, and only one can receive what "
                                          "follows --"));

    if (result.has_diagnostics())
        result.set_outcome(args_outcome::setup_error, args_setup_exit_code);

    return result;
}
