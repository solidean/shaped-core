#include "baseline.hh"

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

/// Insertion sort over a small vector of strings — a library blesses a few dozen headers at most.
void sort_strings(cc::vector<cc::string>& v)
{
    for (auto i = isize(1); i < v.size(); ++i)
        for (auto j = i; j > 0 && precedes_ignoring_case(v[j], v[j - 1]); --j)
            cc::swap(v[j], v[j - 1]);
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
    for (auto i = isize(1); i < groups.size(); ++i)
        for (auto j = i; j > 0 && groups[j].config_path < groups[j - 1].config_path; --j)
            cc::swap(groups[j], groups[j - 1]);

    for (auto& g : groups)
        sort_strings(g.includes);
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
        return cc::string(config_text.subview({.start = 0, .end = begin})) + block
             + cc::string(config_text.subview({.start = tail_from, .end = config_text.size()}));
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
