#include "display_width.hh"

namespace scl::impl
{
namespace
{
/// A UTF-8 continuation byte carries no column of its own — only lead bytes advance the cursor.
bool is_continuation(char c)
{
    return (u8(c) & 0xC0) == 0x80;
}
} // namespace

cc::string expand_tabs(cc::string_view line, i32 tab_width)
{
    if (line.find('\t') < 0)
        return cc::string(line);

    auto out = cc::string::create_with_capacity(line.size() + tab_width);
    i32 column = 0;
    for (isize i = 0; i < line.size(); ++i)
    {
        if (line[i] == '\t')
        {
            auto const stop = tab_width - (column % tab_width);
            for (i32 k = 0; k < stop; ++k)
                out += ' ';
            column += stop;
            continue;
        }

        out += line[i];
        if (!is_continuation(line[i]))
            ++column;
    }
    return out;
}

i32 display_column(cc::string_view line, isize byte_offset, i32 tab_width)
{
    auto const end = byte_offset < 0 ? 0 : (byte_offset > line.size() ? line.size() : byte_offset);

    i32 column = 0;
    for (isize i = 0; i < end; ++i)
    {
        if (line[i] == '\t')
            column += tab_width - (column % tab_width);
        else if (!is_continuation(line[i]))
            ++column;
    }
    return column;
}
} // namespace scl::impl
