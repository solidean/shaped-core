#include "line_tree.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

namespace
{
using namespace sgl;

/// Appends lines to the tree, keeping the path from the top level down to the most recent non-blank line.
struct tree_builder
{
    parsed_file& file;
    /// Open ancestors, innermost last; every entry is indented less than the one after it.
    cc::vector<i32> path;
    /// The most recent child per open level, so a new sibling can be linked without walking a chain.
    /// `last_child[0]` is the top level, `last_child[i + 1]` belongs to `path[i]`.
    cc::vector<i32> last_child;
    /// Blank lines waiting for the next non-blank line to say where they belong.
    cc::vector<i32> pending_blank;

    void link_under_innermost(i32 index)
    {
        auto const parent = path.empty() ? -1 : path.back();
        auto& previous = last_child.back();

        file.lines[index].parent = parent;
        if (previous >= 0)
            file.lines[previous].next_sibling = index;
        else if (parent >= 0)
            file.lines[parent].first_child = index;
        else
            file.first_line = index;
        previous = index;
    }

    void flush_blank()
    {
        for (auto const index : pending_blank)
            link_under_innermost(index);
        pending_blank.clear();
    }

    void add(i32 index)
    {
        auto const& l = file.lines[index];
        if (l.kind == line_kind::blank)
        {
            pending_blank.push_back(index);
            return;
        }

        while (!path.empty() && file.lines[path.back()].indent_columns >= l.indent_columns)
        {
            path.pop_back();
            last_child.pop_back();
        }

        flush_blank();
        link_under_innermost(index);
        path.push_back(index);
        last_child.push_back(-1);
    }

    void finish()
    {
        // Blank lines at the end of the file belong to nobody's body.
        path.clear();
        last_child.resize_down_to(1);
        flush_blank();
    }
};

bool is_indent_char(char c)
{
    return c == ' ' || c == '\t';
}
} // namespace

sgl::parsed_file sgl::build_line_tree(cc::string source)
{
    CC_ASSERT(source.size() <= isize(0xFFFF'FFFFu), "a source file is limited to 4 GiB");

    auto file = parsed_file{.source = cc::move(source)};
    auto const text = cc::string_view(file.source);
    auto const size = text.size();

    auto builder = tree_builder{.file = file};
    builder.last_child.push_back(-1);

    auto at = isize(0);
    while (at < size)
    {
        auto end = at;
        while (end < size && text[end] != '\n' && text[end] != '\r')
            ++end;

        auto terminator = 0;
        if (end < size)
            terminator = text[end] == '\r' && end + 1 < size && text[end + 1] == '\n' ? 2 : 1;

        auto l = line{
            .text = {.offset = u32(at), .length = u32(end - at)},
            .terminator_length = u8(terminator),
        };

        auto indent_end = at;
        auto columns = u32(0);
        while (indent_end < end && is_indent_char(text[indent_end]))
        {
            columns = text[indent_end] == '\t' ? (columns / 4 + 1) * 4 : columns + 1;
            ++indent_end;
        }

        if (indent_end < end)
        {
            l.kind = line_kind::code;
            l.indent_bytes = u32(indent_end - at);
            l.indent_columns = columns;

            for (auto i = at; i < indent_end; ++i)
                if (text[i] == '\t')
                {
                    file.diagnostics.push_back({
                        .kind = diagnostic_kind::tab_in_indentation,
                        .level = default_severity_of(diagnostic_kind::tab_in_indentation),
                        .where = {.offset = u32(i), .length = 1},
                    });
                    break;
                }
        }

        auto const index = i32(file.lines.size());
        file.lines.push_back(l);
        builder.add(index);

        at = end + terminator;
    }

    builder.finish();
    return file;
}
