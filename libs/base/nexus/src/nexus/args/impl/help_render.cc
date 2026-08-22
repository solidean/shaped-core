#include "help_render.hh"

#include <clean-core/platform/console.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <nexus/args/builder.hh>

// Two columns: the spellings on the left, the description wrapped on the right.
// The left column is sized to the widest entry, capped, so one very long option does not push every
// description off the screen.

namespace
{
using nx::isize;
using nx::impl::binding;
using nx::impl::binding_kind;

constexpr isize indent = 2;
constexpr isize gap = 2;
constexpr isize max_name_column = 32;
constexpr isize min_text_column = 20;

/// Break `text` on spaces so no line exceeds `width`, and indent every line after the first.
/// A word longer than the width is emitted whole rather than cut: a path or a URL stays selectable.
void append_wrapped(cc::string& out, cc::string_view text, isize width, isize hanging_indent)
{
    if (text.empty())
        return;

    auto column = isize(0);
    auto start = isize(0);

    while (start < text.size())
    {
        while (start < text.size() && text[start] == ' ')
            ++start;

        if (start >= text.size())
            break;

        auto end = start;
        while (end < text.size() && text[end] != ' ')
            ++end;

        auto const word = text.subview({.start = start, .end = end});

        if (column > 0 && column + 1 + word.size() > width)
        {
            out += "\n";
            out.resize_to_filled(out.size() + hanging_indent, ' ');
            column = 0;
        }

        if (column > 0)
        {
            out += " ";
            ++column;
        }

        out += word;
        column += word.size();
        start = end;
    }
}

/// Wrap `text` while KEEPING the line structure it already has.
///
/// A section is prose the caller wrote, and often carries a worked example whose indentation and line
/// breaks are the content — reflowing it into one paragraph destroys exactly the part worth reading.
/// So each line is wrapped on its own, hanging under its own leading indentation.
void append_block(cc::string& out, cc::string_view text, isize width, isize indent_columns)
{
    auto line_start = isize(0);
    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != '\n')
            continue;

        auto const line = text.subview({.start = line_start, .end = i});
        line_start = i + 1;

        out.resize_to_filled(out.size() + indent_columns, ' ');

        // The line's own indentation becomes the hanging indent, so a wrapped example stays aligned.
        auto leading = isize(0);
        while (leading < line.size() && line[leading] == ' ')
            ++leading;

        out.resize_to_filled(out.size() + leading, ' ');
        append_wrapped(out, line.subview(leading), width - leading, indent_columns + leading);
        out += "\n";
    }
}

void append_padded(cc::string& out, cc::string_view text, isize width)
{
    out += text;
    if (text.size() < width)
        out.resize_to_filled(out.size() + (width - text.size()), ' ');
}

/// `-j, --jobs N` — every visible spelling, shorts first, plus the value placeholder once.
cc::string names_of(binding const& b)
{
    if (b.kind != binding_kind::option)
        return cc::string(b.metavar);

    auto out = cc::string();

    for (auto const& n : b.names)
    {
        if (n.hidden || !n.is_short)
            continue;

        if (!out.empty())
            out += ", ";

        out += n.display();
    }

    for (auto const& n : b.names)
    {
        if (n.hidden || n.is_short)
            continue;

        if (!out.empty())
            out += ", ";

        out += n.display();
    }

    if (b.takes_value && !b.metavar.empty())
        cc::format_append(out, " {}", b.metavar);

    if (b.negatable)
        out += ", --no-...";

    return out;
}

/// The trailing "[default: 4] [env: JOBS]" annotations, which say what happens if the flag is left out.
cc::string annotations_of(binding const& b)
{
    auto out = cc::string();

    if (b.required)
        out += " [required]";

    // A flag that is off unless given is the obvious case and says nothing worth reading; one that is ON
    // by default is the surprising one, and that is the half worth printing.
    auto const is_off_by_default_flag = b.is_bool && !b.takes_value && b.default_text == "false";

    if (b.has_default_text && !b.default_text.empty() && !b.required && !is_off_by_default_flag)
        cc::format_append(out, " [default: {}]", b.default_text);

    if (!b.env.empty())
        cc::format_append(out, " [env: {}]", b.env);

    // Only for an argument that actually takes a value: a bool FLAG publishes its spellings for completion,
    // and listing all eight of them next to `--verbose` is noise rather than help.
    if (b.enumerate_values_fn != nullptr && b.takes_value)
    {
        auto values = cc::vector<cc::string>();
        b.enumerate_values_fn(values);

        if (!values.empty())
        {
            out += " [one of: ";
            for (auto i = isize(0); i < values.size(); ++i)
            {
                if (i > 0)
                    out += ", ";

                out += values[i];
            }
            out += "]";
        }
    }

    if (!b.validator_description.empty())
        cc::format_append(out, " ({})", b.validator_description);

    if (!b.deprecated.empty())
        cc::format_append(out, " [deprecated: {}]", b.deprecated);

    return out;
}

bool is_visible(binding const& b)
{
    return !b.hidden && b.kind != binding_kind::rest;
}

/// The synthesized rows for the flags nobody declares but every program answers to.
cc::vector<cc::pair<cc::string, cc::string>> builtin_rows(nx::args_builder const& builder, bool auto_help, bool auto_version)
{
    auto rows = cc::vector<cc::pair<cc::string, cc::string>>();

    if (auto_help)
    {
        rows.push_back({cc::string("-h"), cc::string("show a short help and exit")});
        rows.push_back({cc::string("--help"), cc::string("show the full help and exit")});
    }

    if (auto_version && !builder.info().version.empty())
        rows.push_back({cc::string("--version"), cc::string("show the version and exit")});

    return rows;
}
} // namespace

cc::string nx::impl::help_renderer::usage(args_builder const& builder)
{
    auto out = cc::format("usage: {}", builder._info.name.empty() ? cc::string_view("program") : builder._info.name);

    auto has_options = false;
    for (auto const& b : builder._bindings)
        if (b.kind == binding_kind::option && is_visible(b))
            has_options = true;

    if (has_options)
        out += " [options]";

    if (!builder._commands.empty())
        out += " <command>";

    // A required option is spelled out rather than hidden inside "[options]": leaving it out is an error,
    // so the usage line has to show it.
    for (auto const& b : builder._bindings)
    {
        if (b.kind != binding_kind::option || !is_visible(b) || !b.required)
            continue;

        cc::format_append(out, " {}", b.canonical);
        if (b.takes_value)
            cc::format_append(out, " {}", b.metavar);
    }

    for (auto const index : builder._positional_order)
    {
        auto const& b = builder._bindings[index];
        if (!is_visible(b))
            continue;

        // The format string has to be a literal — cc::format_string checks it at compile time.
        if (b.accumulates && b.min_count > 0)
            cc::format_append(out, " {}...", b.metavar);
        else if (b.accumulates)
            cc::format_append(out, " [{}...]", b.metavar);
        else if (b.required)
            cc::format_append(out, " {}", b.metavar);
        else
            cc::format_append(out, " [{}]", b.metavar);
    }

    for (auto const& b : builder._bindings)
        if (b.kind == binding_kind::rest)
            cc::format_append(out, " [-- {}]", b.metavar);

    return out;
}

cc::string nx::impl::help_renderer::render(args_builder const& builder, args_render_options const& options, bool full)
{
    auto const& info = builder._info;
    auto out = cc::string();

    if (!info.name.empty())
    {
        out += cc::console::colorize(cc::console::color::bold, info.name, options.color);
        if (!info.version.empty())
            cc::format_append(out, " {}", info.version);

        out += "\n";
    }

    if (!info.description.empty())
    {
        append_wrapped(out, info.description, options.width, 0);
        out += "\n";
    }

    if (full && !info.help.empty())
    {
        out += "\n";
        append_block(out, info.help, options.width, 0);
    }

    out += "\n";
    out += usage(builder);
    out += "\n";

    // --- the argument table -----------------------------------------------------------------------------

    auto rows = cc::vector<cc::pair<cc::string, cc::string>>(); // group-less rendering happens per group below

    auto name_width = isize(0);
    auto const measure = [&](cc::string_view names)
    {
        if (names.size() > name_width && names.size() <= max_name_column)
            name_width = names.size();
    };

    for (auto const& b : builder._bindings)
        if (is_visible(b))
            measure(names_of(b));

    for (auto const& row : builtin_rows(builder, builder._auto_help, builder._auto_version))
        measure(row.first);

    auto const text_column = indent + name_width + gap;
    auto const text_width = options.width - text_column < min_text_column ? min_text_column : options.width - text_column;

    // Groups in declaration order, the unnamed one first.
    auto group_order = cc::vector<cc::string>();
    group_order.push_back(cc::string());
    for (auto const& b : builder._bindings)
    {
        if (!is_visible(b) || b.group.empty())
            continue;

        auto known = false;
        for (auto const& g : group_order)
            if (g == b.group)
                known = true;

        if (!known)
            group_order.push_back(b.group);
    }

    auto const emit_row = [&](cc::string_view names, cc::string_view description, cc::string_view annotations)
    {
        out.resize_to_filled(out.size() + indent, ' ');

        if (names.size() > name_width)
        {
            // Too wide to share a line: give the description its own, rather than shoving the column out.
            out += names;
            out += "\n";
            out.resize_to_filled(out.size() + text_column, ' ');
        }
        else
            append_padded(out, names, name_width + gap);

        auto text = cc::string(description);
        text += annotations;
        append_wrapped(out, text, text_width, text_column);
        out += "\n";
    };

    for (auto const& group : group_order)
    {
        auto any = false;
        for (auto const& b : builder._bindings)
            if (is_visible(b) && b.group == group)
                any = true;

        auto const is_default_group = group.empty();
        if (!any && !(is_default_group && builder._auto_help))
            continue;

        out += "\n";
        auto const heading = is_default_group ? cc::string("options:") : cc::format("{}:", group);
        out += cc::console::colorize(cc::console::color::bold, heading, options.color);
        out += "\n";

        for (auto const& b : builder._bindings)
        {
            if (!is_visible(b) || b.group != group)
                continue;

            emit_row(names_of(b), full && !b.help.empty() ? b.help : b.desc, annotations_of(b));
        }

        if (is_default_group)
            for (auto const& row : builtin_rows(builder, builder._auto_help, builder._auto_version))
                emit_row(row.first, row.second, {});
    }

    // Commands come before the extras, because on a tool that has them they are what the reader came for.
    auto visible_commands = isize(0);
    for (auto const& c : builder._commands)
        if (!c.hidden)
            ++visible_commands;

    if (visible_commands > 0)
    {
        out += "\n";
        out += cc::console::colorize(cc::console::color::bold, "commands:", options.color);
        out += "\n";

        auto command_width = isize(0);
        for (auto const& c : builder._commands)
            if (!c.hidden && c.canonical.size() > command_width && c.canonical.size() <= max_name_column)
                command_width = c.canonical.size();

        for (auto const& c : builder._commands)
        {
            if (c.hidden)
                continue;

            out.resize_to_filled(out.size() + indent, ' ');
            append_padded(out, c.canonical, command_width + gap);

            auto text = cc::string(c.desc);

            // A delegate is opaque by nature, and saying so beats showing an empty option list for it.
            if (c.is_delegate())
                cc::format_append(text, " (see '{} {} --help')", builder._info.name, c.canonical);

            append_wrapped(out, text, options.width - indent - command_width - gap, indent + command_width + gap);
            out += "\n";
        }
    }

    if (!full)
        return out;

    // --- everything only --help shows --------------------------------------------------------------------

    if (!builder._documented_env.empty())
    {
        out += "\n";
        out += cc::console::colorize(cc::console::color::bold, "environment:", options.color);
        out += "\n";

        auto env_width = isize(0);
        for (auto const& e : builder._documented_env)
            if (e.name.size() > env_width && e.name.size() <= max_name_column)
                env_width = e.name.size();

        for (auto const& e : builder._documented_env)
        {
            out.resize_to_filled(out.size() + indent, ' ');
            append_padded(out, e.name, env_width + gap);
            append_wrapped(out, e.desc, options.width - indent - env_width - gap, indent + env_width + gap);
            out += "\n";
        }
    }

    if (!builder._document_validators.empty())
    {
        out += "\n";
        out += cc::console::colorize(cc::console::color::bold, "constraints:", options.color);
        out += "\n";

        for (auto const& rule : builder._document_validators)
        {
            out.resize_to_filled(out.size() + indent, ' ');
            append_wrapped(out, rule.description, options.width - indent, indent);
            out += "\n";
        }
    }

    for (auto const& s : builder._sections)
    {
        out += "\n";
        out += cc::console::colorize(cc::console::color::bold, cc::format("{}:", s.title), options.color);
        out += "\n";
        append_block(out, s.text, options.width - indent, indent);
    }

    if (!builder._examples.empty())
    {
        out += "\n";
        out += cc::console::colorize(cc::console::color::bold, "examples:", options.color);
        out += "\n";

        for (auto const& e : builder._examples)
        {
            if (!e.desc.empty())
            {
                out.resize_to_filled(out.size() + indent, ' ');
                append_wrapped(out, e.desc, options.width - indent, indent);
                out += "\n";
            }

            out.resize_to_filled(out.size() + indent + 2, ' ');
            out += e.command_line;
            out += "\n";
        }
    }

    return out;
}
