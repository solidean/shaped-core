#include "baseline.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>

namespace scl
{
namespace
{
bool equal_ignoring_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (cc::to_lower(a[i]) != cc::to_lower(b[i]))
            return false;
    return true;
}

bool precedes_ignoring_case(cc::string_view a, cc::string_view b)
{
    auto const n = a.size() < b.size() ? a.size() : b.size();
    for (auto i = isize(0); i < n; ++i)
    {
        auto const x = cc::to_lower(a[i]);
        auto const y = cc::to_lower(b[i]);
        if (x != y)
            return x < y;
    }
    return a.size() < b.size();
}

/// The offset just past the line containing `pos`, or the end of the text.
isize end_of_line(cc::string_view text, isize pos)
{
    while (pos < text.size() && text[pos] != '\n')
        ++pos;
    return pos < text.size() ? pos + 1 : pos;
}
} // namespace

void add_to_baseline(cc::vector<baseline_group>& groups, cc::string_view config_path, cc::string_view include)
{
    for (auto& g : groups)
    {
        if (g.config_path != config_path)
            continue;
        for (auto const& i : g.includes)
            if (equal_ignoring_case(i, include))
                return;
        g.includes.push_back(cc::string(include));
        return;
    }

    baseline_group g;
    g.config_path = cc::string(config_path);
    g.includes.push_back(cc::string(include));
    groups.push_back(cc::move(g));
}

void sort_baseline(cc::vector<baseline_group>& groups)
{
    // No ties on either level: add_to_baseline merges on an exact config_path and rejects a case-insensitively
    // equal include, so an unstable sort is enough.
    cc::sort_by(groups, &baseline_group::config_path);

    for (auto& g : groups)
        cc::sort(g.includes, [](cc::string_view a, cc::string_view b) { return precedes_ignoring_case(a, b); });
}

cc::string render_baseline_block(cc::span<cc::string const> includes)
{
    cc::string out;
    out += k_baseline_begin;
    out += '\n';
    for (auto const& i : includes)
        out += cc::format("  - kind: allow-include\n    value: {}\n    reason: baseline\n", i);
    out += k_baseline_end;
    out += '\n';
    return out;
}

cc::string apply_baseline_block(cc::string_view config_text, cc::span<cc::string const> includes)
{
    auto const begin = config_text.find(k_baseline_begin);
    auto const block = includes.empty() ? cc::string() : render_baseline_block(includes);

    if (begin >= 0)
    {
        auto const end_marker = config_text.find(k_baseline_end, begin);
        auto const tail_from = end_marker < 0 ? config_text.size() : end_of_line(config_text, end_marker);
        auto out = cc::string(config_text);
        out.replace({.start = begin, .end = tail_from}, block);
        return out;
    }

    if (includes.empty())
        return cc::string(config_text);

    auto out = cc::string(config_text);
    if (!out.empty() && out[out.size() - 1] != '\n')
        out += '\n';

    // A file that never had a `rules:` key needs one before a list item can hang off it.
    if (config_text.find("rules:") < 0)
        out += "rules:\n";

    return out + block;
}
} // namespace scl
