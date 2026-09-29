#include "text_index.hh"

using namespace cc::primitive_defines;

namespace
{
/// The number of bytes of the UTF-8 sequence led by `lead`; a stray continuation byte counts as one.
[[nodiscard]] isize sequence_length(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    if (lead >= 0xC0)
        return 2;
    return 1;
}
} // namespace

lsp::text_index::text_index(cc::string_view text)
{
    _line_starts.push_back(0);
    for (auto i = isize(0); i < text.size(); ++i)
    {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
        {
            ++i;
            _line_starts.push_back(i + 1);
        }
        else if (text[i] == '\n' || text[i] == '\r')
            _line_starts.push_back(i + 1);
    }
}

isize lsp::text_index::line_end(cc::string_view text, isize line) const
{
    auto end = line + 1 < _line_starts.size() ? _line_starts[line + 1] : text.size();
    // strip the terminator, which is one byte or `\r\n`
    if (end > _line_starts[line] && line + 1 < _line_starts.size())
    {
        --end;
        if (end > _line_starts[line] && text[end] == '\n' && text[end - 1] == '\r')
            --end;
    }
    return end;
}

lsp::position lsp::text_index::position_of(cc::string_view text, isize offset, position_encoding e) const
{
    offset = offset < 0 ? 0 : offset > text.size() ? text.size() : offset;
    // the last line starting at or before `offset`
    auto lo = isize(0);
    auto hi = _line_starts.size();
    while (hi - lo > 1)
    {
        auto const mid = lo + (hi - lo) / 2;
        if (_line_starts[mid] <= offset)
            lo = mid;
        else
            hi = mid;
    }
    auto const start = _line_starts[lo];
    if (e == position_encoding::utf8)
        return {.line = i32(lo), .character = i32(offset - start)};

    auto units = i32(0);
    for (auto i = start; i < offset;)
    {
        auto const n = sequence_length(static_cast<unsigned char>(text[i]));
        units += n == 4 ? 2 : 1;
        i += n;
    }
    return {.line = i32(lo), .character = units};
}

isize lsp::text_index::offset_of(cc::string_view text, position p, position_encoding e) const
{
    if (p.line < 0)
        return 0;
    if (p.line >= _line_starts.size())
        return text.size();
    auto const start = _line_starts[p.line];
    auto const end = line_end(text, p.line);
    if (e == position_encoding::utf8)
    {
        auto const at = start + isize(p.character < 0 ? 0 : p.character);
        return at > end ? end : at;
    }

    auto units = i32(0);
    auto i = start;
    while (i < end && units < p.character)
    {
        auto const n = sequence_length(static_cast<unsigned char>(text[i]));
        units += n == 4 ? 2 : 1;
        i += n;
    }
    return i > end ? end : i;
}
