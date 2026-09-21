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
    cc::vector<line_id> path;
    /// The most recent child per open level, so a new sibling can be linked without walking a chain.
    /// `last_child[0]` is the top level, `last_child[i + 1]` belongs to `path[i]`.
    cc::vector<line_id> last_child;
    /// Blank lines waiting for the next non-blank line to say where they belong.
    cc::vector<line_id> pending_blank;

    void link_under_innermost(line_id id)
    {
        auto const parent = path.empty() ? line_id::none : path.back();
        auto& previous = last_child.back();

        file.at(id).parent = parent;
        if (is_valid(previous))
            file.at(previous).next_sibling = id;
        else if (is_valid(parent))
            file.at(parent).first_child = id;
        else
            file.first_line = id;
        previous = id;
    }

    void flush_blank()
    {
        for (auto const id : pending_blank)
            link_under_innermost(id);
        pending_blank.clear();
    }

    /// Appends `added` to the file and links it into the tree.
    void push(line const& added)
    {
        file.lines.push_back(added);
        auto const id = line_id(i32(file.lines.size() - 1));

        auto const& l = file.at(id);
        if (l.kind == line_kind::blank)
        {
            pending_blank.push_back(id);
            return;
        }

        while (!path.empty() && file.at(path.back()).indent_columns >= l.indent_columns)
        {
            path.pop_back();
            last_child.pop_back();
        }

        flush_blank();
        link_under_innermost(id);
        path.push_back(id);
        last_child.push_back(line_id::none);
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
    builder.last_child.push_back(line_id::none);

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

        // A byte-order mark is indentation of no width: kept in the line, so the source prints back, and never a token.
        auto indent_end = at;
        if (at == 0 && text.starts_with("\xEF\xBB\xBF"))
            indent_end = 3;
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

        builder.push(l);

        at = end + terminator;
    }

    builder.finish();
    return file;
}
