#include "builder.hh"

#include <clean-core/platform/console.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/impl/help_render.hh>
#include <nexus/args/impl/parse_engine.hh>
#include <nexus/args/impl/setup_check.hh>

nx::args_builder nx::args(app_info info)
{
    return args_builder(info);
}

nx::args_builder::args_builder(app_info info) : _info(info)
{
}
nx::args_builder::~args_builder() = default;
nx::args_builder::args_builder(args_builder&&) noexcept = default;
nx::args_builder& nx::args_builder::operator=(args_builder&&) noexcept = default;

namespace
{
/// Split a declared spelling into "is it short" and "what is its text".
/// A spelling carrying its own dashes is taken verbatim, which is the only way to declare a one-character
/// LONG name such as `--x`; everything else follows the length rule.
nx::impl::arg_name classify_name(nx::arg::name_spec const& spec)
{
    auto const text = spec.text;

    if (text.starts_with("--"))
        return {.text = cc::string(text.subview(2)), .is_short = false, .hidden = spec.hidden};

    if (text.starts_with('-') && text.size() > 1)
        return {.text = cc::string(text.subview(1)), .is_short = true, .hidden = spec.hidden};

    return {.text = cc::string(text), .is_short = text.size() == 1, .hidden = spec.hidden};
}
} // namespace

void nx::args_builder::configure(impl::binding& b, cc::span<arg::name_spec const> names, impl::common_options opts)
{
    for (auto const& spec : names)
        b.names.push_back(classify_name(spec));

    // The first long spelling names the argument in diagnostics, falling back to the first short one.
    for (auto const& name : b.names)
        if (!name.is_short)
        {
            b.canonical = name.display();
            break;
        }

    if (b.canonical.empty() && !b.names.empty())
        b.canonical = b.names.front().display();

    b.desc = cc::string(opts.desc);
    b.help = cc::string(opts.help);
    b.group = cc::string(opts.group.empty() ? cc::string_view(_current_group) : opts.group);
    b.env = cc::string(opts.env);
    b.deprecated = cc::string(opts.deprecated);
    b.required = opts.required;
    b.negatable = opts.negatable;
    b.hidden = opts.hidden;
    b.complete = opts.complete;

    b.metavar = cc::string(opts.metavar.empty() ? cc::string_view(b.type_name) : opts.metavar);

    if (!opts.default_text.empty())
    {
        b.default_text = cc::string(opts.default_text);
        b.has_default_text = true;
    }
}

void nx::args_builder::configure_positional(impl::binding& b, cc::string_view metavar, impl::common_options opts)
{
    configure(b, {}, opts);

    b.metavar = cc::string(metavar.empty() ? cc::string_view(b.type_name) : metavar);
    b.canonical = b.metavar;
    b.min_count = opts.min_count;
    b.max_count = opts.max_count;

    // A fixed positional is required unless it was given a default, which the variable's own value is.
    if (!b.accumulates && opts.required)
        b.required = true;
}

nx::args_builder& nx::args_builder::add_option(impl::binding b)
{
    _bindings.push_back(cc::move(b));
    return *this;
}

nx::args_builder& nx::args_builder::add_positional(impl::binding b)
{
    _bindings.push_back(cc::move(b));
    _positional_order.push_back(_bindings.size() - 1);
    return *this;
}

nx::args_builder& nx::args_builder::action(cc::span<arg::name_spec const> names,
                                           cc::unique_function<void()> fn,
                                           arg_options<bool> opts)
{
    auto b = impl::binding();
    b.takes_value = false;
    b.type_name = "";
    b.action = cc::move(fn);
    configure(b, names, impl::to_common(opts));
    return add_option(cc::move(b));
}

nx::args_builder& nx::args_builder::action(cc::span<arg::name_spec const> names,
                                           cc::unique_function<void()> fn,
                                           cc::string_view desc)
{
    return action(names, cc::move(fn), arg_options<bool>{.desc = desc});
}

nx::args_builder& nx::args_builder::value_action(cc::span<arg::name_spec const> names,
                                                 cc::unique_function<void(cc::string_view)> fn,
                                                 arg_options<cc::string> opts)
{
    auto b = impl::binding();
    b.takes_value = true;
    b.type_name = "STRING";
    b.value_action = cc::move(fn);
    configure(b, names, impl::to_common(opts));
    return add_option(cc::move(b));
}

nx::args_builder& nx::args_builder::rest(cc::vector<cc::string_view>& target, cc::string_view metavar, cc::string_view desc)
{
    auto b = impl::binding();
    b.kind = impl::binding_kind::rest;
    b.target = &target;
    b.metavar = cc::string(metavar);
    b.canonical = cc::string(metavar);
    b.desc = cc::string(desc);
    b.group = _current_group;
    _bindings.push_back(cc::move(b));
    return *this;
}

nx::args_builder& nx::args_builder::group(cc::string_view title)
{
    _current_group = cc::string(title);
    return *this;
}

nx::args_builder& nx::args_builder::section(cc::string_view title, cc::string_view text)
{
    _sections.push_back({.title = cc::string(title), .text = cc::string(text)});
    return *this;
}

nx::args_builder& nx::args_builder::example(cc::string_view command_line, cc::string_view desc)
{
    _examples.push_back({.command_line = cc::string(command_line), .desc = cc::string(desc)});
    return *this;
}

nx::args_builder& nx::args_builder::document_env(cc::string_view name, cc::string_view desc)
{
    _documented_env.push_back({.name = cc::string(name), .desc = cc::string(desc)});
    return *this;
}

nx::args_builder& nx::args_builder::allow_unknown(cc::vector<cc::string_view>& target)
{
    _unknown_target = &target;
    return *this;
}

nx::args_builder& nx::args_builder::stop_at_first_positional()
{
    _stop_at_first_positional = true;
    return *this;
}

nx::args_builder& nx::args_builder::no_auto_help()
{
    _auto_help = false;
    return *this;
}

nx::args_builder& nx::args_builder::no_auto_version()
{
    _auto_version = false;
    return *this;
}

nx::args_builder& nx::args_builder::no_auto_print()
{
    _auto_print = false;
    return *this;
}

nx::args_builder& nx::args_builder::full_help_on_error()
{
    _full_help_on_error = true;
    return *this;
}

nx::args_builder& nx::args_builder::exact_long_names()
{
    _normalize_underscores = false;
    return *this;
}

nx::args_builder& nx::args_builder::usage_exit_code(int code)
{
    _usage_exit_code = code;
    return *this;
}

nx::args_result nx::args_builder::parse(int argc, char const* const* argv)
{
    // argv[0] is the program path, never an argument.
    auto tokens = cc::vector<cc::string_view>();
    for (auto i = 1; i < argc; ++i)
        tokens.push_back(cc::string_view(argv[i]));

    return parse(tokens);
}

nx::args_result nx::args_builder::parse(cc::span<cc::string_view const> tokens)
{
    return impl::parse_engine::run(*this, tokens);
}

nx::args_result nx::args_builder::validate_setup() const
{
    return impl::setup_checker::run(*this);
}

cc::string nx::args_builder::help_text(args_render_options const& options) const
{
    return impl::help_renderer::render(*this, options, true);
}

cc::string nx::args_builder::short_help_text(args_render_options const& options) const
{
    return impl::help_renderer::render(*this, options, false);
}

cc::string nx::args_builder::usage_line() const
{
    return impl::help_renderer::usage(*this);
}

void nx::args_builder::print_help() const
{
    cc::println(help_text({.color = cc::console::color_enabled(), .width = cc::console::terminal_width().value_or(100)}));
}

void nx::args_builder::print_short_help() const
{
    cc::println(
        short_help_text({.color = cc::console::color_enabled(), .width = cc::console::terminal_width().value_or(100)}));
}
