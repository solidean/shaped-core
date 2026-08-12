#include "glob.hh"

namespace scl
{
namespace
{
/// Backtracking matcher over the two views.
/// Recursion depth is bounded by the number of `*` groups in the pattern, which is a handful.
bool match_from(cc::string_view p, cc::string_view s)
{
    isize pi = 0;
    isize si = 0;

    while (pi < p.size())
    {
        auto const c = p[pi];

        if (c == '*')
        {
            auto const globstar = pi + 1 < p.size() && p[pi + 1] == '*';
            auto rest = p.subview(pi + (globstar ? 2 : 1));

            // `**/x` must also match a bare `x`, so the separator behind a globstar is optional.
            if (globstar && rest.starts_with('/') && match_from(rest.subview(1), s.subview(si)))
                return true;

            // Try every split point, shortest first.
            // A plain `*` stops at the first `/`; a `**` does not.
            for (auto k = si;; ++k)
            {
                if (match_from(rest, s.subview(k)))
                    return true;
                if (k >= s.size())
                    return false;
                if (!globstar && s[k] == '/')
                    return false;
            }
        }

        if (si >= s.size())
            return false;

        if (c == '?')
        {
            if (s[si] == '/')
                return false;
        }
        else if (c != s[si])
            return false;

        ++pi;
        ++si;
    }

    return si == s.size();
}
} // namespace

cc::string normalize_path(cc::string_view path)
{
    cc::string out;
    for (auto const c : path)
    {
        auto const n = c == '\\' ? '/' : c;
        if (n == '/' && !out.empty() && out[out.size() - 1] == '/')
            continue;
        out += n;
    }
    // A trailing slash would make `libs/base` and `libs/base/` compare unequal as directory keys.
    while (out.size() > 1 && out[out.size() - 1] == '/')
        out.resize_down_to(out.size() - 1);
    return out;
}

bool glob_matches(cc::string_view pattern, cc::string_view path)
{
    if (pattern.ends_with('/'))
    {
        auto const subtree = cc::string(pattern) + "**";
        return match_from(subtree, path);
    }
    return match_from(pattern, path);
}
} // namespace scl
