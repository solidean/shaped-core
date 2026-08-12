#include "glob.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/char_predicates.hh>

namespace cc
{
namespace
{
/// Backtracking matcher over the two views.
/// Recursion depth is bounded by the number of `*` groups in the pattern, which is a handful.
bool match_from(cc::string_view p, cc::string_view s, bool fold_case)
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
            if (globstar && rest.starts_with('/') && match_from(rest.subview(1), s.subview(si), fold_case))
                return true;

            // Try every split point, shortest first.
            // A plain `*` stops at the first `/`; a `**` does not.
            for (auto k = si;; ++k)
            {
                if (match_from(rest, s.subview(k), fold_case))
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
        else if (fold_case ? cc::to_lower(c) != cc::to_lower(s[si]) : c != s[si])
            return false;

        ++pi;
        ++si;
    }

    return si == s.size();
}

bool is_letter(char c)
{
    return cc::is_lower(c) || cc::is_upper(c);
}

} // namespace

cc::string glob_normalize_path(cc::string_view path)
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

    // The MSYS spelling of a drive, `/c/x`, becomes the native `c:/x`.
    // Unconditional, on every platform: a path here is only ever compared against another, and a one-letter root
    // directory is vanishingly rare next to a git-bash path reaching a Windows tool.
    if (out.size() >= 2 && out[0] == '/' && is_letter(out[1]) && (out.size() == 2 || out[2] == '/'))
    {
        cc::string native;
        native += out[1];
        native += ':';
        native += cc::string_view(out).subview(2);
        out = cc::move(native);
    }

    // The drive letter is the one part of a path whose case carries no information, so it does not get to make two spellings differ.
    if (out.size() >= 2 && out[1] == ':' && is_letter(out[0]))
        out[0] = cc::to_lower(out[0]);

    return out;
}

bool glob_matches(cc::string_view pattern, cc::string_view path, cc::flags<glob_option> options)
{
    auto const fold_case = options.has(glob_option::ignore_case);

    // Read before normalizing, which is what drops the trailing slash the shorthand is spelled with.
    auto const subtree = pattern.ends_with('/');

    // The rewritten spellings must outlive the views handed to the matcher.
    cc::string owned_pattern;
    cc::string owned_path;
    if (options.has(glob_option::normalize))
    {
        owned_pattern = glob_normalize_path(pattern);
        owned_path = glob_normalize_path(path);
        pattern = owned_pattern;
        path = owned_path;
    }

    if (subtree)
    {
        owned_pattern = cc::string(pattern);
        if (!cc::string_view(owned_pattern).ends_with('/'))
            owned_pattern += '/';
        owned_pattern += "**";
        pattern = owned_pattern;
    }

    return match_from(pattern, path, fold_case);
}
} // namespace cc
