#include "source_cache.hh"

#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/char_predicates.hh>

namespace itrace
{
namespace
{
cc::string_view trim(cc::string_view s)
{
    while (!s.empty() && cc::is_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && cc::is_space(s.back()))
        s.remove_suffix(1);
    return s;
}
} // namespace

cc::vector<cc::string> const& source_cache::lines_of(cc::string_view path)
{
    auto entry = _files.entry(path);
    if (entry.exists())
        return entry.value();

    auto lines = cc::vector<cc::string>();

    // The adapter owns the buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_value())
    {
        auto stream = adapter.value().stream();
        auto line = cc::string();
        while (stream.read_line(line).value_or(false)) // a read error ends the loop like an unreadable file
            lines.push_back(line);
    }

    // An unreadable file caches as empty, so we do not retry it per instruction.
    return entry.get_or_emplace(cc::move(lines));
}

cc::string_view source_cache::line(cc::string_view path, u32 line_number)
{
    if (path.empty() || line_number == 0)
        return {};

    auto const& lines = lines_of(path);
    if (isize(line_number) > lines.size())
        return {};

    return trim(lines[isize(line_number) - 1]);
}

cc::string_view source_cache::raw_line(cc::string_view path, u32 line_number)
{
    if (path.empty() || line_number == 0)
        return {};

    auto const& lines = lines_of(path);
    if (isize(line_number) > lines.size())
        return {};

    return lines[isize(line_number) - 1]; // read_line already dropped the CR of a CRLF ending
}

u32 source_cache::line_count(cc::string_view path)
{
    if (path.empty())
        return 0;
    return u32(lines_of(path).size());
}
} // namespace itrace
