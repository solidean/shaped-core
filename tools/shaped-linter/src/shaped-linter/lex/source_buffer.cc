#include "source_buffer.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

namespace scl
{
source_buffer source_buffer::from_text(cc::string text, cc::string_view path, u32 file_id)
{
    source_buffer b;
    b._text = cc::move(text);
    b._path = cc::string(path);
    b._file_id = file_id;
    b.build_line_index();
    return b;
}

void source_buffer::build_line_index()
{
    _line_starts.clear();
    _line_starts.push_back(0); // line 1 starts at offset 0, even for empty text

    auto const sv = cc::string_view(_text);
    for (isize i = 0; i < sv.size(); ++i)
        if (sv[i] == '\n')
            _line_starts.push_back(u32(i + 1)); // the char after '\n' opens the next line
}

cc::string_view source_buffer::span_text(source_span s) const
{
    CC_ASSERT(s.file_id == _file_id, "span refers to a different file");
    auto const sv = cc::string_view(_text);
    CC_ASSERT(s.byte_begin <= s.byte_end && s.byte_end <= sv.size(), "span out of bounds");
    return sv.subview({.start = isize(s.byte_begin), .end = isize(s.byte_end)});
}

line_col source_buffer::line_col_at(u32 byte_offset) const
{
    auto const n = u32(cc::string_view(_text).size());
    auto const off = byte_offset < n ? byte_offset : n;

    // Largest line-start index <= off. Binary search over the sorted _line_starts.
    isize lo = 0;
    isize hi = _line_starts.size(); // first index whose start is > off
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        if (_line_starts[mid] <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    auto const line_index = lo - 1; // lo is the first start > off, so the line is one back

    return {
        .line = u32(line_index + 1),
        .column = off - _line_starts[line_index] + 1,
    };
}

cc::string_view source_buffer::line_text(u32 byte_offset) const
{
    auto const sv = cc::string_view(_text);
    auto const n = u32(sv.size());
    auto const off = byte_offset < n ? byte_offset : n;

    isize begin = off;
    while (begin > 0 && sv[begin - 1] != '\n' && sv[begin - 1] != '\r')
        --begin;

    isize end = off;
    while (end < sv.size() && sv[end] != '\n' && sv[end] != '\r')
        ++end;

    return sv.subview({.start = begin, .end = end});
}
} // namespace scl
