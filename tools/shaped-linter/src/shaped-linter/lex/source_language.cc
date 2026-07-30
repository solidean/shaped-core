#include "source_language.hh"

namespace scl
{
namespace
{
/// The extension of `path` including its dot, lowercased where it matters, or empty if it has none.
/// A dot in a directory component does not count, so `a.b/readme` has no extension.
cc::string_view extension_of(cc::string_view path)
{
    for (auto i = path.size(); i > 0; --i)
    {
        auto const c = path[i - 1];
        if (c == '/' || c == '\\')
            break;
        if (c == '.')
            return path.subview({.start = i - 1, .end = path.size()});
    }
    return {};
}
} // namespace

source_language language_from_path(cc::string_view path)
{
    auto const ext = extension_of(path);
    if (ext == ".py" || ext == ".pyi")
        return source_language::python;
    if (ext == ".md")
        return source_language::markdown;
    return source_language::cpp;
}

cc::string_view source_language_name(source_language l)
{
    switch (l)
    {
    case source_language::cpp:
        return "cpp";
    case source_language::python:
        return "python";
    case source_language::markdown:
        return "markdown";
    }
    return "unknown";
}
} // namespace scl
