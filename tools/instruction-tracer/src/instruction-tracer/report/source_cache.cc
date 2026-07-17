#include "source_cache.hh"

#include <clean-core/string/char_predicates.hh>

#include <fstream>
#include <string>

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

    cc::vector<cc::string> lines;

    // clean-core has no file I/O; std::ifstream is the seam.
    std::ifstream file{std::string(path.data(), size_t(path.size()))};
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
            lines.push_back(cc::string(cc::string_view(line.data(), isize(line.size()))));
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
} // namespace itrace
