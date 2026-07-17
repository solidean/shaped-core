#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
namespace
{
bool is_separator(char c)
{
    return c == '/' || c == '\\';
}
} // namespace
} // namespace slib::impl

cc::optional<cc::string> slib::impl::normalize_path(cc::string_view path)
{
    cc::vector<cc::string_view> segments;

    isize begin = 0;
    for (isize i = 0; i <= path.size(); ++i)
    {
        // a segment ends at a separator or at the end of the path
        if (i < path.size() && !is_separator(path[i]))
            continue;

        auto const segment = path.subview({.start = begin, .end = i});
        begin = i + 1;

        if (segment.empty() || segment == ".") // repeated separators and '.' carry no meaning
            continue;

        if (segment == "..")
        {
            if (segments.empty())
                return cc::nullopt; // climbed past the root
            segments.remove_back();
            continue;
        }

        segments.push_back(segment);
    }

    cc::string result;
    for (auto const& segment : segments)
    {
        if (!result.empty())
            result.push_back('/');
        result.append(segment);
    }
    return result;
}

cc::optional<cc::string> slib::impl::join_path(cc::string_view base, cc::string_view relative)
{
    if (base.empty())
        return normalize_path(relative);

    auto joined = cc::string::create_copy_of(base);
    joined.push_back('/');
    joined.append(relative);
    return normalize_path(joined);
}

cc::string_view slib::impl::parent_path(cc::string_view path)
{
    auto const separator = path.rfind('/');
    if (separator < 0)
        return {};
    return path.subview({.start = 0, .end = separator});
}

bool slib::impl::is_path_under(cc::string_view path, cc::string_view prefix)
{
    if (prefix.empty()) // the root
        return true;
    if (!path.starts_with(prefix))
        return false;
    // Guard the segment boundary, so "foobar" does not count as living under "foo".
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

cc::string_view slib::impl::relative_to(cc::string_view path, cc::string_view prefix)
{
    CC_ASSERT(is_path_under(path, prefix), "path must live under prefix");

    if (prefix.empty())
        return path;
    if (path.size() == prefix.size())
        return {};                          // the prefix itself
    return path.subview(prefix.size() + 1); // skip the separator
}
