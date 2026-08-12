#include "config_resolver.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>

namespace scl
{
namespace
{
/// The directory holding `path`, normalized, or empty when there is none left to climb to.
///
/// A relative path climbs one step further than its first component, to `.` — the working directory.
/// Without that a run given `libs/base/clean-core/x.cc` would stop at `libs` and never reach the repo-root
/// config, while the same run given absolute paths would find it.
cc::string_view parent_of(cc::string_view path)
{
    auto const slash = path.rfind('/');
    if (slash > 0)
        return path.subview({.start = 0, .end = slash});
    if (slash == 0)
        return {}; // the leading separator of an absolute posix path

    // No separator left: a drive (`C:`) is the top, and anything else is a relative first component.
    if (path == "." || path.contains(':'))
        return {};
    return ".";
}

/// Read a whole file, or nothing when it is not there.
/// A missing config is the normal case and must not look like a failure.
cc::optional<cc::string> read_text_from_disk(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};

    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return {};

    auto const& b = bytes.value();
    return cc::string(cc::string_view(reinterpret_cast<char const*>(b.data()), b.size()));
}
} // namespace

config_resolver::config_resolver() : _read_file([](cc::string_view path) { return read_text_from_disk(path); })
{
}

config_resolver::config_resolver(cc::unique_function<cc::optional<cc::string>(cc::string_view)> read_file)
  : _read_file(cc::move(read_file))
{
}

lint_config const& config_resolver::for_directory(cc::string_view dir)
{
    if (auto const* cached = _by_directory.get_ptr(dir))
        return *cached;

    // The parent first, so the merged list comes out root-first and the nearest entry decides.
    lint_config merged;
    auto const parent = parent_of(dir);
    if (!parent.empty())
        merged = for_directory(parent);

    auto const config_path = cc::format("{}/{}", dir, k_config_file_name);
    if (auto const text = _read_file(config_path); text.has_value())
    {
        auto directives = load_include_directives(text.value(), dir);
        if (directives.has_error())
            _errors.push_back(cc::format("{}: {}", config_path, directives.error().to_string()));
        else
            for (auto& d : directives.value())
                merged.include_directives.push_back(cc::move(d));

        merged.nearest_config_path = config_path;
    }

    _by_directory[cc::string(dir)] = cc::move(merged);
    return _by_directory.get(dir);
}

lint_config const& config_resolver::resolve(cc::string_view file_path)
{
    auto const normalized = cc::glob_normalize_path(file_path);
    auto const dir = parent_of(normalized);

    static lint_config const empty;
    if (dir.empty())
        return empty; // a bare file name has no directory to search above it

    return for_directory(dir);
}
} // namespace scl
