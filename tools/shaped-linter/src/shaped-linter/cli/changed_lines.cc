#include "changed_lines.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

namespace scl
{
namespace
{
cc::string normalized(cc::string_view path)
{
    auto out = cc::string(path);
    out.replace_all('\\', '/');
    return out;
}

cc::result<u32> parse_number(cc::string_view s)
{
    if (s.empty())
        return cc::error("expected a line number");

    u64 value = 0;
    for (auto const c : s)
    {
        if (c < '0' || c > '9')
            return cc::error(cc::format("'{}' is not a line number", s));
        value = value * 10 + u64(c - '0');
        if (value > 0xFFFF'FFFF)
            return cc::error(cc::format("line number '{}' is out of range", s));
    }
    return u32(value);
}
} // namespace

bool changed_lines::covers(cc::string_view path, u32 line) const
{
    auto const wanted = normalized(path);
    for (auto const& f : _files)
    {
        if (cc::string_view(f.path) != cc::string_view(wanted))
            continue;
        for (auto const& r : f.ranges)
            if (line >= r.first && line <= r.last)
                return true;
        return false;
    }
    return false;
}

cc::result<changed_lines> parse_changed_lines(cc::string_view text)
{
    changed_lines out;

    auto pos = isize(0);
    auto line_number = 0;
    while (pos < text.size())
    {
        auto end = pos;
        while (end < text.size() && text[end] != '\n')
            ++end;
        auto content_end = end;
        if (content_end > pos && text[content_end - 1] == '\r')
            --content_end;

        auto const line = text.subview({.start = pos, .end = content_end});
        pos = end < text.size() ? end + 1 : end;
        ++line_number;

        if (line.empty())
            continue;

        // A Windows path carries its own ':' after the drive letter, so the ranges start at the LAST one.
        auto const colon = line.rfind(':');
        if (colon <= 0)
            return cc::error(cc::format("changed-lines line {}: expected '<path>:<ranges>', got '{}'", line_number, line));

        auto ranges = changed_lines::file_ranges{.path = normalized(line.subview({.start = 0, .end = colon}))};

        auto const spec = line.subview(colon + 1);
        auto range_begin = isize(0);
        while (range_begin <= spec.size())
        {
            auto range_end = range_begin;
            while (range_end < spec.size() && spec[range_end] != ',')
                ++range_end;

            auto const item = spec.subview({.start = range_begin, .end = range_end});
            range_begin = range_end + 1;
            if (item.empty())
                continue;

            auto const dash = item.find('-');
            auto first = parse_number(dash < 0 ? item : item.subview({.start = 0, .end = dash}));
            if (first.has_error())
                return cc::error(cc::format("changed-lines line {}: {}", line_number, first.error().to_string()));
            auto last = dash < 0 ? first.value() : u32(0);
            if (dash >= 0)
            {
                auto parsed = parse_number(item.subview(dash + 1));
                if (parsed.has_error())
                    return cc::error(cc::format("changed-lines line {}: {}", line_number, parsed.error().to_string()));
                last = parsed.value();
            }

            ranges.ranges.push_back({.first = first.value(), .last = last});
        }

        // A path may be listed more than once; the ranges simply union.
        auto merged = false;
        for (auto& existing : out._files)
            if (cc::string_view(existing.path) == cc::string_view(ranges.path))
            {
                for (auto const& r : ranges.ranges)
                    existing.ranges.push_back(r);
                merged = true;
                break;
            }
        if (!merged)
            out._files.push_back(cc::move(ranges));
    }

    return out;
}
} // namespace scl
