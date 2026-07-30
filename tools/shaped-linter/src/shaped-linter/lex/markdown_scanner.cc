#include "markdown_scanner.hh"

namespace scl
{
namespace
{
/// Leading spaces of `line`, counting a tab as one column — a fence's indent limit is all this feeds.
isize indent_of(cc::string_view line)
{
    isize i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    return i;
}

/// The run length if `line` opens or closes a fence at `indent`, else 0.
/// A fence is three or more of the same character, indented at most three columns.
isize fence_run(cc::string_view line, isize indent, char& out_char)
{
    if (indent > 3 || indent >= line.size())
        return 0;

    auto const c = line[indent];
    if (c != '`' && c != '~')
        return 0;

    isize run = 0;
    while (indent + run < line.size() && line[indent + run] == c)
        ++run;
    if (run < 3)
        return 0;

    out_char = c;
    return run;
}

bool is_blank(cc::string_view line)
{
    for (auto const c : line)
        if (c != ' ' && c != '\t')
            return false;
    return true;
}
} // namespace

cc::vector<markdown_line> scan_markdown(source_buffer const& buffer)
{
    cc::vector<markdown_line> out;

    // The open fence, if we are inside one: its character and the run length a closer must match or exceed.
    char open_char = 0;
    isize open_run = 0;

    for (u32 line = 1; line <= buffer.line_count(); ++line)
    {
        auto const span = buffer.line_span(line);
        auto const text = buffer.span_text(span);
        auto const indent = indent_of(text);

        char c = 0;
        auto const run = fence_run(text, indent, c);

        if (open_run > 0)
        {
            // Inside a fence, only a matching closer of at least the opener's length ends it.
            // Everything
            // else is code — including a fence of the other character, which is how a ~~~ block may hold ```.
            auto const closes = run > 0 && c == open_char && run >= open_run;
            out.push_back({.kind = closes ? markdown_line_kind::fence : markdown_line_kind::code, .span = span});
            if (closes)
                open_run = 0;
            continue;
        }

        if (run > 0)
        {
            open_char = c;
            open_run = run;
            out.push_back({.kind = markdown_line_kind::fence, .span = span});
            continue;
        }

        if (is_blank(text))
        {
            out.push_back({.kind = markdown_line_kind::blank, .span = span});
            continue;
        }

        // A pipe table is laid out in columns, so its cells are not prose lines and the usual reading of a
        // line break does not apply to them.
        auto const table = indent < text.size() && text[indent] == '|';
        out.push_back({.kind = table ? markdown_line_kind::table : markdown_line_kind::text, .span = span});
    }

    return out;
}
} // namespace scl
