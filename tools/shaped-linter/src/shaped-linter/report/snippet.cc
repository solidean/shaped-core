#include "snippet.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
#include <shaped-linter/report/impl/display_width.hh>


namespace scl
{
namespace
{
using cc::console::color;

constexpr color primary_color = color::yellow;
constexpr color secondary_color = color::cyan;

/// One underline segment on one line, in display columns (0-based, half-open).
/// `text` is set only on the label's last line, where the label's text belongs.
struct mark
{
    i32 begin_col = 0;
    i32 end_col = 0;
    bool primary = false;
    cc::string_view text;
};

/// A line carrying at least one underline.
/// Context lines are derived from these, never stored.
struct marked_line
{
    u32 line = 1;
    cc::vector<mark> marks;
};

/// Everything to render for one file: where its header points, which lines to show, and what to underline.
struct file_block
{
    u32 file_id = 0;
    u32 header_line = 1;
    u32 header_column = 1;
    cc::vector<marked_line> marked;
    cc::vector<u32> display_lines; // ascending, unique; marked lines plus their context
};

i32 digits_of(u32 v)
{
    i32 d = 1;
    while (v >= 10)
    {
        v /= 10;
        ++d;
    }
    return d;
}

void add_unique(cc::vector<u32>& v, u32 value)
{
    for (auto const existing : v)
        if (existing == value)
            return;
    v.push_back(value);
}

/// The entry for `line`, created if this is its first mark.
/// Order is restored once, after collection.
marked_line& line_entry(cc::vector<marked_line>& lines, u32 line)
{
    for (auto& l : lines)
        if (l.line == line)
            return l;

    return lines.emplace_back(marked_line{.line = line});
}

/// The lines of `span` that get an underline.
/// A span longer than `max_span_lines` keeps only its first and last two lines — the middle is elided, and the gap renders as a `...` row.
cc::vector<u32> underlined_lines(u32 first, u32 last, i32 max_span_lines)
{
    cc::vector<u32> out;
    if (i64(last) - i64(first) + 1 <= max_span_lines)
    {
        for (auto l = first; l <= last; ++l)
            out.push_back(l);
        return out;
    }

    out.push_back(first);
    out.push_back(first + 1);
    out.push_back(last - 1);
    out.push_back(last);
    return out;
}

void add_label(file_block& block, label const& l, bool primary, source_manager const& sm, report_style style)
{
    auto const& buffer = sm.buffer(l.span.file_id);

    auto const begin = buffer.line_col_at(l.span.byte_begin);
    auto end = buffer.line_col_at(l.span.byte_end);

    // A span ending exactly at a line start belongs to the line before it, not to the empty head of the next.
    if (end.line > begin.line && end.column == 1)
    {
        end.line -= 1;
        end.column = u32(buffer.line_text(end.line).size()) + 1;
    }

    for (auto const line : underlined_lines(begin.line, end.line, style.max_span_lines))
    {
        auto const text = buffer.line_text(line);
        auto const first_byte = line == begin.line ? isize(begin.column) - 1 : 0;
        auto const last_byte = line == end.line ? isize(end.column) - 1 : text.size();

        auto const begin_col = impl::display_column(text, first_byte, style.tab_width);
        auto end_col = impl::display_column(text, last_byte, style.tab_width);
        if (end_col <= begin_col)
            end_col = begin_col + 1; // an empty span still needs one caret to point with

        line_entry(block.marked, line)
            .marks.push_back({
                .begin_col = begin_col,
                .end_col = end_col,
                .primary = primary,
                .text = line == end.line ? cc::string_view(l.text) : "",
            });
    }
}

cc::vector<file_block> build_blocks(cc::span<label const> labels, source_manager const& sm, report_style style)
{
    cc::vector<file_block> blocks;

    for (isize i = 0; i < labels.size(); ++i)
    {
        auto const& l = labels[i];

        file_block* block = nullptr;
        for (auto& b : blocks)
            if (b.file_id == l.span.file_id)
                block = &b;

        if (block == nullptr)
        {
            auto const loc = sm.resolve(l.span);
            blocks.push_back({.file_id = l.span.file_id, .header_line = loc.line, .header_column = loc.column});
            block = &blocks.back();
        }

        add_label(*block, l, i == 0, sm, style);
    }

    for (auto& b : blocks)
    {
        auto const line_count = sm.buffer(b.file_id).line_count();
        for (auto const& m : b.marked)
        {
            auto const first = m.line > u32(style.context_lines) ? m.line - u32(style.context_lines) : 1;
            auto const last = m.line + u32(style.context_lines);
            for (auto l = first; l <= last && l <= line_count; ++l)
                add_unique(b.display_lines, l);
        }

        // Collection order follows the labels; the render walks the file top to bottom.
        cc::sort(b.marked, [](marked_line const& x, marked_line const& y) { return x.line < y.line; });
        for (auto& m : b.marked)
            cc::sort(m.marks, [](mark const& x, mark const& y) { return x.begin_col < y.begin_col; });
        cc::sort(b.display_lines);
    }

    return blocks;
}

/// Builds one output line left to right.
/// Padding is counted in visible columns, so it stays correct however many invisible escape bytes the colored pieces add.
struct row
{
    cc::string out;
    i32 col = 0;

    void pad_to(i32 target)
    {
        while (col < target)
        {
            out += ' ';
            ++col;
        }
    }

    void put(cc::string_view text, color c, bool colored)
    {
        out += cc::console::colorize(c, text, colored);
        col += i32(text.size()); // callers pass ASCII runs they built themselves
    }
};

cc::string_view trim_right(cc::string_view s)
{
    auto end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        --end;
    return s.subview({.start = 0, .end = end});
}

/// `   12 |` — right-aligned in the gutter, with no trailing space so an empty row stays empty.
cc::string gutter(cc::string_view number, i32 width, bool colored)
{
    auto text = cc::string();
    for (auto i = i32(number.size()); i < width; ++i)
        text += ' ';
    text += number;
    text += " |";
    return cc::console::colorize(color::dim, text, colored);
}

void append_line(cc::string& out, cc::string_view number, i32 width, cc::string_view content, bool colored)
{
    out += gutter(number, width, colored);
    auto const trimmed = trim_right(content);
    if (!trimmed.empty())
    {
        out += ' ';
        out += trimmed;
    }
    out += '\n';
}

color color_of(mark const& m)
{
    return m.primary ? primary_color : secondary_color;
}

/// The underline itself, plus the label text.
/// One labelled mark prints its text inline; several become a rustc-style ladder, since two texts cannot share a line.
void append_underlines(cc::string& out, cc::vector<mark> const& marks, i32 width, bool colored)
{
    auto labelled = cc::vector<mark const*>();
    for (auto const& m : marks)
        if (!m.text.empty())
            labelled.push_back(&m);

    auto underline = row();
    for (auto const& m : marks)
    {
        underline.pad_to(m.begin_col);
        underline.put(cc::string::create_filled(m.end_col - m.begin_col, m.primary ? '^' : '-'), color_of(m), colored);
    }

    if (labelled.size() == 1)
    {
        underline.out += ' ';
        underline.out += cc::console::colorize(color_of(*labelled[0]), labelled[0]->text, colored);
    }
    append_line(out, "", width, underline.out, colored);

    if (labelled.size() < 2)
        return;

    // One rung of `|` per labelled mark, then the texts from right to left under their own marks.
    auto rungs = row();
    for (auto const* m : labelled)
    {
        rungs.pad_to(m->begin_col);
        rungs.put("|", color_of(*m), colored);
    }
    append_line(out, "", width, rungs.out, colored);

    for (isize i = labelled.size() - 1; i >= 0; --i)
    {
        auto r = row();
        for (isize j = 0; j < i; ++j)
        {
            r.pad_to(labelled[j]->begin_col);
            r.put("|", color_of(*labelled[j]), colored);
        }
        r.pad_to(labelled[i]->begin_col);
        r.out += cc::console::colorize(color_of(*labelled[i]), labelled[i]->text, colored);
        append_line(out, "", width, r.out, colored);
    }
}
} // namespace

cc::string render_snippet(cc::span<label const> labels, source_manager const& sm, report_style style)
{
    if (labels.empty())
        return {};

    auto const blocks = build_blocks(labels, sm, style);

    u32 max_line = 1;
    for (auto const& b : blocks)
        for (auto const l : b.display_lines)
            if (l > max_line)
                max_line = l;
    auto const width = digits_of(max_line);

    auto out = cc::string();
    for (isize i = 0; i < blocks.size(); ++i)
    {
        auto const& block = blocks[i];
        auto const& buffer = sm.buffer(block.file_id);

        // `-->` points at the finding itself; a further file is a `:::` aside, as in rustc.
        for (auto k = 0; k < width; ++k)
            out += ' ';
        out += cc::console::colorize(color::dim, i == 0 ? "-->" : ":::", style.color);
        out += cc::format(" {}:{}:{}\n", buffer.path(), block.header_line, block.header_column);

        append_line(out, "", width, "", style.color);

        u32 previous = 0;
        for (auto const line : block.display_lines)
        {
            if (previous != 0 && line > previous + 1)
                out += cc::console::colorize(color::dim, "...\n", style.color);
            previous = line;

            append_line(out, cc::to_string(line), width, impl::expand_tabs(buffer.line_text(line), style.tab_width),
                        style.color);

            for (auto const& m : block.marked)
                if (m.line == line)
                    append_underlines(out, m.marks, width, style.color);
        }

        append_line(out, "", width, "", style.color);
    }

    return out;
}
} // namespace scl
