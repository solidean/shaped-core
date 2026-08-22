#include "parse_engine.hh"

#include <clean-core/platform/console.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/builder.hh>
#include <nexus/args/impl/suggest.hh>

// The walk is one pass, left to right, accumulating diagnostics rather than stopping at the first.
// Positional VALUES are collected during the pass and assigned afterwards, because a variadic positional
// can only be sized once the total is known — the fixed ones around it are back-filled from both ends.

namespace
{
using nx::isize;
using nx::impl::binding;
using nx::impl::binding_kind;
using bindings_t = cc::vector<binding>;

/// `_` and `-` are the same character when looking up a long name, unless that was turned off.
/// Compared on the fly rather than by rewriting the token, so a diagnostic still quotes what was typed.
bool long_name_matches(cc::string_view declared, cc::string_view typed, bool normalize)
{
    if (declared.size() != typed.size())
        return false;

    for (auto i = isize(0); i < declared.size(); ++i)
    {
        auto a = declared[i];
        auto b = typed[i];
        if (normalize)
        {
            if (a == '_')
                a = '-';
            if (b == '_')
                b = '-';
        }

        if (a != b)
            return false;
    }

    return true;
}

/// A token like `-5` is a value, not a cluster — unless the program really did declare a short `-5`.
bool looks_like_negative_number(cc::string_view token)
{
    return token.size() >= 2 && token[0] == '-' && cc::is_digit(token[1]);
}

binding* find_long_in(bindings_t& bindings, cc::string_view name, bool normalize, bool globals_only)
{
    for (auto& b : bindings)
    {
        if (b.kind != binding_kind::option || (globals_only && !b.is_global))
            continue;

        for (auto const& n : b.names)
            if (!n.is_short && long_name_matches(n.text, name, normalize))
                return &b;
    }

    return nullptr;
}

binding* find_short_in(bindings_t& bindings, char c, bool globals_only)
{
    for (auto& b : bindings)
    {
        if (b.kind != binding_kind::option || (globals_only && !b.is_global))
            continue;

        for (auto const& n : b.names)
            if (n.is_short && n.text.size() == 1 && n.text[0] == c)
                return &b;
    }

    return nullptr;
}

struct parse_state
{
    nx::args_result result;
    cc::vector<cc::string_view> positional_values;
    bool help_requested = false;
    bool short_help_requested = false;
    bool version_requested = false;

    void add(nx::diagnostic_kind kind,
             isize index,
             cc::string_view token,
             cc::string_view arg_name,
             cc::string message,
             cc::string suggestion = {})
    {
        result.add_diagnostic({
            .kind = kind,
            .source = {.origin = nx::arg_origin::command_line, .index = index},
            .token = cc::string(token),
            .arg_name = cc::string(arg_name),
            .message = cc::move(message),
            .suggestion = cc::move(suggestion),
        });
    }
};

/// Hand one token to a binding, reporting a conversion failure against the argument that refused it.
///
/// The diagnostic's `token` is the VALUE, not the option it followed: the value is the offending text, and
/// which option it belonged to is already in `arg_name`.
bool apply_value(parse_state& state, binding& b, cc::string_view value, isize index)
{
    if (b.value_action.is_valid())
    {
        b.value_action(value);
        return true;
    }

    auto error = cc::string();
    if (b.parse_fn != nullptr && !b.parse_fn(b.target, value, error))
    {
        auto const reason = error.empty() ? cc::string("the value was not accepted") : error;
        state.add(nx::diagnostic_kind::invalid_value, index, value, b.canonical,
                  cc::format("invalid value '{}' for {}: {}", value, b.canonical, reason));
        return false;
    }

    // The rule runs per occurrence, right here, so the complaint can quote the token that broke it rather
    // than reporting the variable's final value long after the line has been forgotten.
    if (b.validate.is_valid())
    {
        auto rule_error = cc::string();
        if (!b.validate(b.target, rule_error))
        {
            auto const reason = rule_error.empty() ? b.validator_description : rule_error;
            state.add(nx::diagnostic_kind::failed_validation, index, value, b.canonical,
                      cc::format("invalid value '{}' for {}: {}", value, b.canonical, reason));
            return false;
        }
    }

    return true;
}

/// Positional values are matched to their slots only once the whole line is known.
/// With a variadic in the mix the fixed slots are filled from both ends and it takes what is left, which
/// is what makes `app SRC files... DEST` mean what it looks like.
void assign_positionals(bindings_t& bindings, cc::span<isize const> order, parse_state& state)
{
    auto const& values = state.positional_values;

    auto variadic_slot = isize(-1);
    for (auto k = isize(0); k < order.size(); ++k)
        if (bindings[order[k]].accumulates)
            variadic_slot = k;

    if (variadic_slot < 0)
    {
        for (auto k = isize(0); k < values.size(); ++k)
        {
            if (k >= order.size())
            {
                state.add(nx::diagnostic_kind::unexpected_positional, -1, values[k], {},
                          cc::format("unexpected argument '{}'", values[k]));
                continue;
            }

            apply_value(state, bindings[order[k]], values[k], -1);
        }

        for (auto k = values.size(); k < order.size(); ++k)
        {
            auto const& b = bindings[order[k]];
            if (b.required)
                state.add(nx::diagnostic_kind::missing_positional, -1, {}, b.metavar,
                          cc::format("missing required argument {}", b.metavar));
        }

        return;
    }

    auto const before = variadic_slot;
    auto const after = order.size() - variadic_slot - 1;

    if (values.size() < before + after)
    {
        // Not even the fixed slots can be filled, so name each one that goes without.
        for (auto k = values.size(); k < order.size(); ++k)
        {
            auto const& b = bindings[order[k]];
            if (b.required && !b.accumulates)
                state.add(nx::diagnostic_kind::missing_positional, -1, {}, b.metavar,
                          cc::format("missing required argument {}", b.metavar));
        }
    }

    auto const variadic_count = values.size() > before + after ? values.size() - before - after : isize(0);
    auto next = isize(0);

    for (auto k = isize(0); k < before && next < values.size(); ++k, ++next)
        apply_value(state, bindings[order[k]], values[next], -1);

    auto& variadic = bindings[order[variadic_slot]];
    for (auto k = isize(0); k < variadic_count; ++k, ++next)
        apply_value(state, variadic, values[next], -1);

    for (auto k = variadic_slot + 1; k < order.size() && next < values.size(); ++k, ++next)
        apply_value(state, bindings[order[k]], values[next], -1);

    if (variadic_count < variadic.min_count)
        state.add(
            nx::diagnostic_kind::missing_positional, -1, {}, variadic.metavar,
            cc::format("{} needs at least {} value(s), got {}", variadic.metavar, variadic.min_count, variadic_count));

    if (variadic.max_count >= 0 && variadic_count > variadic.max_count)
        state.add(
            nx::diagnostic_kind::unexpected_positional, -1, {}, variadic.metavar,
            cc::format("{} takes at most {} value(s), got {}", variadic.metavar, variadic.max_count, variadic_count));
}

/// What fills in for an argument the command line never mentioned: the environment, then make_default.
/// A required argument that neither could satisfy is the last thing reported.
void apply_fallbacks(bindings_t& bindings, parse_state& state)
{
    for (auto& b : bindings)
    {
        if (b.kind == binding_kind::rest)
            continue;

        if (b.occurrences > 0)
        {
            if (!b.deprecated.empty())
                cc::eprintln(cc::format("warning: {} is deprecated: {}", b.canonical, b.deprecated));

            continue;
        }

        if (!b.env.empty())
        {
            if (auto const value = cc::environment_variable(b.env); value.has_value())
            {
                auto error = cc::string();
                if (b.parse_fn != nullptr && !b.parse_fn(b.target, value.value(), error))
                {
                    auto const reason = error.empty() ? cc::string("the value was not accepted") : error;
                    state.result.add_diagnostic({
                        .kind = nx::diagnostic_kind::invalid_value,
                        .source = {.origin = nx::arg_origin::environment, .name = b.env},
                        .token = value.value(),
                        .arg_name = b.canonical,
                        .message = cc::format("invalid value '{}' for {}: {}", value.value(), b.canonical, reason),
                    });
                }

                continue;
            }
        }

        if (b.apply_make_default.is_valid())
        {
            b.apply_make_default();
            continue;
        }

        if (b.required && b.kind == binding_kind::option)
            state.add(nx::diagnostic_kind::missing_required, -1, {}, b.canonical,
                      cc::format("{} is required", b.canonical));
    }
}
} // namespace

namespace
{
/// A negatable bool also answers to `--no-<name>`, for every long spelling it has.
binding* find_negated(nx::args_builder& builder, cc::string_view name, bool normalize)
{
    if (!name.starts_with("no-"))
        return nullptr;

    auto* const b = nx::impl::parse_engine::find_long(builder, name.subview(3), normalize);
    return (b != nullptr && b->negatable) ? b : nullptr;
}

/// Would every character of `-abc` resolve as a short option?
/// Asked BEFORE walking, because `-force` begins with a perfectly good `-f` and would otherwise be
/// reported as four unknown letters instead of as the long name it obviously is.
/// A value taker ends the question: it swallows whatever remains, so nothing after it has to resolve.
bool cluster_fully_resolves(nx::args_builder& builder, cc::string_view token)
{
    for (auto k = isize(1); k < token.size(); ++k)
    {
        auto* const b = nx::impl::parse_engine::find_short(builder, token[k]);
        if (b == nullptr)
            return false;

        if (b->takes_value)
            return true;
    }

    return true;
}
} // namespace

nx::impl::binding* nx::impl::parse_engine::find_long(args_builder& builder, cc::string_view name, bool normalize)
{
    // This level first: a subcommand that declares its own --output means its own, not the parent's.
    if (auto* const own = find_long_in(builder._bindings, name, normalize, false); own != nullptr)
        return own;

    for (auto* up = builder._parent; up != nullptr; up = up->_parent)
        if (auto* const inherited = find_long_in(up->_bindings, name, normalize, true); inherited != nullptr)
            return inherited;

    return nullptr;
}

nx::impl::binding* nx::impl::parse_engine::find_short(args_builder& builder, char c)
{
    if (auto* const own = find_short_in(builder._bindings, c, false); own != nullptr)
        return own;

    for (auto* up = builder._parent; up != nullptr; up = up->_parent)
        if (auto* const inherited = find_short_in(up->_bindings, c, true); inherited != nullptr)
            return inherited;

    return nullptr;
}

cc::vector<cc::string> nx::impl::parse_engine::suggestion_space(args_builder& builder, bool want_long)
{
    // Hidden aliases are included on purpose: a deprecated name is exactly what somebody half-remembers,
    // so leaving it out would be the wrong kind of tidy.
    auto out = cc::vector<cc::string>();

    auto const collect = [&](bindings_t const& bindings, bool globals_only)
    {
        for (auto const& b : bindings)
        {
            if (b.kind != binding_kind::option || (globals_only && !b.is_global))
                continue;

            for (auto const& n : b.names)
                if (n.is_short != want_long)
                    out.push_back(n.display());
        }
    };

    collect(builder._bindings, false);
    for (auto* up = builder._parent; up != nullptr; up = up->_parent)
        collect(up->_bindings, true);

    return out;
}

nx::impl::command_node* nx::impl::parse_engine::find_command(args_builder& builder, cc::string_view name)
{
    for (auto& node : builder._commands)
        for (auto const& n : node.names)
            if (n.text == name)
                return &node;

    return nullptr;
}

nx::args_builder& nx::impl::parse_engine::force_declare(args_builder& builder, command_node& node)
{
    // Idempotent on purpose: help forces every subtree, a parse forces the selected one, and completion
    // forces the lot again.
    // Declaring twice would double every argument.
    if (!node.declared)
    {
        auto child = cc::make_unique<args_builder>(app_info{.name = node.canonical, .description = node.desc});
        child->_parent = &builder;
        child->_auto_print = builder._auto_print;
        child->_normalize_underscores = builder._normalize_underscores;
        child->_usage_exit_code = builder._usage_exit_code;

        if (node.declare.is_valid())
            node.declare(*child);

        builder._subtrees.push_back(cc::move(child));
        node.subtree = builder._subtrees.size() - 1;
        node.declared = true;
    }

    return *builder._subtrees[node.subtree];
}

nx::args_result nx::impl::parse_engine::run(args_builder& builder, cc::span<cc::string_view const> tokens)
{
    // A declaration bug is not the user's problem and must not be reported as one, so it short-circuits.
    if (auto setup = builder.validate_setup(); setup.should_exit())
    {
        if (builder._auto_print)
            cc::eprintln(setup.diagnostic_text({.color = cc::console::color_enabled()}));

        return setup;
    }

    // Own every token, so a bound cc::string_view outlives the caller's argv.
    // Reserved exactly once: a reallocation partway through would leave earlier views pointing at freed memory.
    builder._token_storage.clear();
    builder._tokens.clear();
    builder._raw.clear();

    auto total = isize(0);
    for (auto const& t : tokens)
        total += t.size();

    builder._token_storage.reserve(total);
    for (auto const& t : tokens)
    {
        auto const offset = builder._token_storage.size();
        for (auto const c : t)
            builder._token_storage.push_back(c);

        builder._tokens.push_back(cc::string_view(builder._token_storage.data() + offset, t.size()));
    }

    auto& bindings = builder._bindings;
    for (auto& b : bindings)
        b.occurrences = 0;

    builder._command_path.clear();

    auto state = parse_state();
    auto const& owned = builder._tokens;
    auto const count = owned.size();
    auto const normalize = builder._normalize_underscores;
    auto options_ended = false;

    for (auto i = isize(0); i < count; ++i)
    {
        auto const token = owned[i];

        if (!options_ended && token == "--")
        {
            for (auto j = i + 1; j < count; ++j)
                builder._raw.push_back(owned[j]);

            auto* rest_binding = static_cast<binding*>(nullptr);
            for (auto& b : bindings)
                if (b.kind == binding_kind::rest)
                    rest_binding = &b;

            if (rest_binding != nullptr)
                *static_cast<cc::vector<cc::string_view>*>(rest_binding->target) = builder._raw;
            else
                state.add(diagnostic_kind::unexpected_separator, i, token, {},
                          "'--' was given but this program declares nothing to receive what follows it");

            break;
        }

        auto const is_long = !options_ended && token.starts_with("--") && token.size() > 2;

        if (is_long)
        {
            auto const body = token.subview(2);
            auto const eq = body.find('=');
            auto const name = eq >= 0 ? body.subview({.start = 0, .end = eq}) : body;
            auto const has_value = eq >= 0;
            auto const inline_value = has_value ? body.subview(eq + 1) : cc::string_view();

            if (builder._auto_help && name == "help" && find_long(builder, "help", normalize) == nullptr)
            {
                state.help_requested = true;
                break;
            }

            if (builder._auto_version && !builder._info.version.empty() && name == "version"
                && find_long(builder, "version", normalize) == nullptr)
            {
                state.version_requested = true;
                break;
            }

            auto* b = find_long(builder, name, normalize);
            auto negated = false;
            if (b == nullptr)
            {
                b = find_negated(builder, name, normalize);
                negated = b != nullptr;
            }

            if (b == nullptr)
            {
                if (builder._unknown_target != nullptr)
                {
                    builder._unknown_target->push_back(token);
                    continue;
                }

                auto const space = suggestion_space(builder, true);
                state.add(diagnostic_kind::unknown_option, i, token, {}, cc::format("unknown option '--{}'", name),
                          best_suggestion(cc::format("--{}", name), space));
                continue;
            }

            ++b->occurrences;

            if (negated)
            {
                if (has_value)
                    state.add(diagnostic_kind::unexpected_value, i, token, b->canonical,
                              cc::format("'--{}' takes no value", name));
                else if (b->set_bool_fn != nullptr)
                    b->set_bool_fn(b->target, false);

                continue;
            }

            if (b->action.is_valid())
            {
                if (has_value)
                    state.add(diagnostic_kind::unexpected_value, i, token, b->canonical,
                              cc::format("{} takes no value", b->canonical));
                else
                    b->action();

                continue;
            }

            if (b->counting)
            {
                if (has_value)
                    state.add(diagnostic_kind::unexpected_value, i, token, b->canonical,
                              cc::format("{} takes no value", b->canonical));
                else if (b->add_count_fn != nullptr)
                    b->add_count_fn(b->target, 1);

                continue;
            }

            if (b->is_bool && !b->takes_value)
            {
                // The one place a value is optional, and only because '=' makes it unambiguous.
                if (has_value)
                    apply_value(state, *b, inline_value, i);
                else if (b->set_bool_fn != nullptr)
                    b->set_bool_fn(b->target, true);

                continue;
            }

            if (has_value)
            {
                apply_value(state, *b, inline_value, i);
                continue;
            }

            if (i + 1 >= count)
            {
                state.add(diagnostic_kind::missing_value, i, token, b->canonical,
                          cc::format("{} needs a value", b->canonical));
                continue;
            }

            ++i;
            apply_value(state, *b, owned[i], i);
            continue;
        }

        auto const is_cluster = !options_ended && token.size() >= 2 && token[0] == '-' && token != "-"
                             && !(looks_like_negative_number(token) && find_short(builder, token[1]) == nullptr);

        if (is_cluster)
        {
            if (builder._auto_help && token == "-h" && find_short(builder, 'h') == nullptr)
            {
                state.short_help_requested = true;
                break;
            }

            // The classic slip: a long name typed with one dash.
            // Said once, rather than as a complaint about each of its letters.
            // Only when the token does not also read as a real cluster, which a deliberate `-abc` still does.
            if (!cluster_fully_resolves(builder, token) && find_long(builder, token.subview(1), normalize) != nullptr)
            {
                if (builder._unknown_target != nullptr)
                    builder._unknown_target->push_back(token);
                else
                    state.add(diagnostic_kind::unknown_option, i, token, {},
                              cc::format("'{}' is not a cluster of short options", token),
                              cc::format("--{}", token.subview(1)));

                continue;
            }

            for (auto k = isize(1); k < token.size(); ++k)
            {
                auto const c = token[k];
                auto* const b = find_short(builder, c);

                if (b == nullptr)
                {
                    if (builder._unknown_target != nullptr)
                    {
                        builder._unknown_target->push_back(token);
                        break;
                    }

                    auto const space = suggestion_space(builder, false);
                    state.add(diagnostic_kind::unknown_option, i, token, {}, cc::format("unknown option '-{}'", c),
                              best_suggestion(cc::format("-{}", c), space));
                    break;
                }

                ++b->occurrences;

                if (b->action.is_valid())
                {
                    b->action();
                    continue;
                }

                if (b->counting)
                {
                    if (b->add_count_fn != nullptr)
                        b->add_count_fn(b->target, 1);

                    continue;
                }

                if (!b->takes_value)
                {
                    if (b->set_bool_fn != nullptr)
                        b->set_bool_fn(b->target, true);

                    continue;
                }

                // A value taker consumes the rest of the cluster, which is what makes -j4 and -j 4 the same.
                auto const remainder = token.subview(k + 1);

                if (remainder.starts_with('='))
                {
                    state.add(diagnostic_kind::invalid_value, i, token, b->canonical,
                              cc::format("a short option takes its value directly ('-{}{}') or as the next argument "
                                         "('-{} {}'), not with '='",
                                         c, remainder.subview(1), c, remainder.subview(1)));
                    break;
                }

                if (!remainder.empty())
                {
                    apply_value(state, *b, remainder, i);
                    break;
                }

                if (i + 1 >= count)
                {
                    state.add(diagnostic_kind::missing_value, i, token, b->canonical,
                              cc::format("{} needs a value", b->canonical));
                    break;
                }

                ++i;
                apply_value(state, *b, owned[i], i);
                break;
            }

            continue;
        }

        // A bare word is a subcommand when this level declares any: commands and positionals are mutually
        // exclusive, so there is never a question of which one it is.
        if (!options_ended && !builder._commands.empty())
        {
            if (builder._auto_help_command && token == "help" && find_command(builder, "help") == nullptr)
            {
                // `app help build` is what people try first, and it means `app build --help`.
                if (i + 1 < count)
                {
                    if (auto* const target = find_command(builder, owned[i + 1]);
                        target != nullptr && !target->is_delegate())
                    {
                        auto& child = force_declare(builder, *target);
                        auto const child_tokens = cc::vector<cc::string_view>{"--help"};
                        return run(child, child_tokens);
                    }
                }

                state.help_requested = true;
                break;
            }

            auto* const node = find_command(builder, token);
            if (node == nullptr)
            {
                auto candidates = cc::vector<cc::string>();
                for (auto const& c : builder._commands)
                    for (auto const& n : c.names)
                        candidates.push_back(n.text);

                state.add(diagnostic_kind::unknown_command, i, token, {}, cc::format("unknown command '{}'", token),
                          best_suggestion(token, candidates));
                break;
            }

            auto tail = cc::vector<cc::string_view>();
            for (auto j = i + 1; j < count; ++j)
                tail.push_back(owned[j]);

            if (node->is_delegate())
            {
                // Untouched: --help and -- past this point belong to whoever we are handing off to.
                builder._command_path.push_back(node->canonical);
                return args_result(args_outcome::success, node->delegate(tail));
            }

            auto& child = force_declare(builder, *node);
            auto child_result = run(child, tail);

            builder._command_path.push_back(node->canonical);
            for (auto const& part : child._command_path)
                builder._command_path.push_back(part);

            return child_result;
        }

        state.positional_values.push_back(token);
        if (builder._stop_at_first_positional)
            options_ended = true;
    }

    // --- help and version win outright, before anything is reported as wrong ---------------------------

    auto const render = args_render_options{.color = cc::console::color_enabled(),
                                            .width = cc::console::terminal_width().value_or(100)};

    if (state.help_requested || state.short_help_requested)
    {
        builder._help_requested = state.help_requested;
        builder._short_help_requested = state.short_help_requested;

        if (builder._auto_print)
            cc::println(state.help_requested ? builder.help_text(render) : builder.short_help_text(render));

        return args_result(args_outcome::help_requested, 0);
    }

    if (state.version_requested)
    {
        builder._version_requested = true;
        if (builder._auto_print)
            cc::println(cc::format("{} {}", builder._info.name, builder._info.version));

        return args_result(args_outcome::version_requested, 0);
    }

    if (!builder._commands.empty() && builder._command_path.empty() && !state.result.has_diagnostics())
    {
        if (!builder._default_command.empty())
        {
            if (auto* const node = find_command(builder, builder._default_command); node != nullptr)
            {
                auto& child = force_declare(builder, *node);
                auto child_result = run(child, {});
                builder._command_path.push_back(node->canonical);
                return child_result;
            }
        }
        else
            state.add(diagnostic_kind::unknown_command, -1, {}, {}, "a command is required");
    }

    assign_positionals(bindings, builder._positional_order, state);
    apply_fallbacks(bindings, state);

    // Only over a command line that is otherwise sound: a cross-argument rule read against half-bound
    // variables reports something that is not true, on top of an error that is.
    if (!state.result.has_diagnostics())
    {
        for (auto const& rule : builder._document_validators)
        {
            if (rule.check.is_valid() && !rule.check())
                state.add(diagnostic_kind::failed_validation, -1, {}, {}, cc::string(rule.description));
        }
    }

    if (state.result.has_diagnostics())
    {
        state.result.set_outcome(args_outcome::usage_error, builder._usage_exit_code);

        if (builder._auto_print)
        {
            auto const hint = builder._full_help_on_error
                                ? builder.help_text(render)
                                : cc::format("{}\ntry '{} --help' for more", builder.usage_line(), builder._info.name);
            cc::eprintln(render_diagnostics(state.result.diagnostics(), hint, render));
        }
    }

    return cc::move(state.result);
}
