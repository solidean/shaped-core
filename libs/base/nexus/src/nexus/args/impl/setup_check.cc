#include "setup_check.hh"

#include <clean-core/string/format.hh>
#include <nexus/args/builder.hh>
#include <nexus/args/impl/parse_engine.hh>

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

// A default command is dispatched with the tokens the root did not consume, so both levels see the same line
// and a name claimed by both would resolve differently depending on where the walk happened to stop.
// Checking it means declaring the command, which is why this whole pass takes the builder mutably.
void nx::impl::setup_checker::check_default_command(args_builder& builder, args_result& result)
{
    if (builder._default_command.empty())
        return;

    auto* const node = parse_engine::find_command(builder, builder._default_command);
    if (node == nullptr)
    {
        result.add_diagnostic(setup_error(
            cc::format("the default command '{}' is not one of this program's commands", builder._default_command)));
        return;
    }

    if (node->is_delegate())
    {
        result.add_diagnostic(setup_error(cc::format("the default command '{}' is a delegate, which has no declaration "
                                                     "to run against an empty command line",
                                                     builder._default_command)));
        return;
    }

    auto const& child = parse_engine::force_declare(builder, *node);

    for (auto const& mine : builder._bindings)
    {
        if (mine.kind != binding_kind::option)
            continue;

        for (auto const& theirs : child._bindings)
        {
            if (theirs.kind != binding_kind::option)
                continue;

            for (auto const& a : mine.names)
                for (auto const& b : theirs.names)
                    if (a.is_short == b.is_short && a.text == b.text)
                        result.add_diagnostic(setup_error(
                            cc::format("{} is claimed by both this program and its default command '{}', so which one "
                                       "an unnamed invocation means is undecidable — spell the command out, or rename "
                                       "one of them",
                                       a.display(), builder._default_command),
                            mine.canonical));
        }
    }
}

nx::args_result nx::impl::setup_checker::run(args_builder& builder)
{
    auto result = args_result();

    auto variadic_positionals = isize(0);
    auto rest_bindings = isize(0);

    for (auto i = isize(0); i < builder._bindings.size(); ++i)
    {
        auto const& b = builder._bindings[i];

        // Neither of these depends on the kind, so both are asked before the kind splits the checks up.
        if (b.is_global && b.kind != binding_kind::option)
            result.add_diagnostic(setup_error(cc::format("'{}' is marked global but is not a named option, so there is "
                                                         "no depth for it to be "
                                                         "reachable at",
                                                         b.canonical),
                                              b.canonical));

        // An env fallback is read through the binding's parse thunk, which an action, a value_action and a
        // rest binding all lack — so declaring one there would silently do nothing.
        if (!b.env.empty() && b.parse_fn == nullptr)
            result.add_diagnostic(setup_error(cc::format("'{}' names the environment variable {} but has nothing to "
                                                         "parse a value into",
                                                         b.canonical, b.env),
                                              b.canonical));

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

    if (!builder._commands.empty() && !builder._positional_order.empty())
        result.add_diagnostic(setup_error("this level declares both subcommands and positionals, so a bare word could "
                                          "be either"));

    for (auto i = isize(0); i < builder._commands.size(); ++i)
        for (auto j = i + 1; j < builder._commands.size(); ++j)
            for (auto const& a : builder._commands[i].names)
                for (auto const& b : builder._commands[j].names)
                    if (a.text == b.text)
                        result.add_diagnostic(setup_error(cc::format("two commands both claim '{}'", a.text)));

    check_default_command(builder, result);

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
