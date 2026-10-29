#include <clean-core/common/macros.hh>  // CC_OS_WINDOWS
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/uri_resolver.hh>

namespace sv
{
namespace
{
/// The caller's hook, unset until `set_resolve_uri` is called.
///
/// It lives here rather than in the header so the only way to reach it is the setter, exactly as `g_acquire_context`
/// does — nothing can read it, and no translation unit can race the others to initialize it.
/// Unsynchronized on purpose: it is installed once during startup and only read after, and a lock held across the
/// resolver would deadlock the moment a resolver resolves something itself.
uri_resolver_provider g_resolve_uri;

/// The value one hex digit names; `cc` has `is_hex_digit` but no nibble decoder yet (issue: feat-cc-hex-value).
[[nodiscard]] int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

[[nodiscard]] bool is_separator(char c)
{
    return c == '/' || c == '\\';
}

/// Whether `uri` names its own location rather than one relative to something else.
[[nodiscard]] bool is_absolute(cc::string_view uri)
{
    if (uri.empty())
        return false;
    if (is_separator(uri[0]))
        return true;

    // A Windows drive letter, which is absolute despite carrying no scheme.
    if (uri.size() >= 2 && uri[1] == ':')
        return true;

    // A scheme: letters, digits, `+`, `-`, `.`, then a colon.
    // Checked after the drive letter above, so a single-letter "scheme" stays a drive letter.
    for (auto i = isize(0); i < uri.size(); ++i)
    {
        auto const c = uri[i];
        if (c == ':')
            return i > 1;
        auto const schemely = cc::is_alphanumeric(c) || c == '+' || c == '-' || c == '.';
        if (!schemely)
            return false;
    }
    return false;
}

} // namespace

void set_resolve_uri(uri_resolver_provider provider)
{
    g_resolve_uri = cc::move(provider);
}

cc::result<cc::pinned_data<byte const>> resolve_uri(cc::string_view uri)
{
    return g_resolve_uri ? g_resolve_uri(uri) : impl::resolve_uri_from_filesystem(uri);
}

uri_resolver_provider filesystem_resolver(filesystem_resolver_options options)
{
    return [max_bytes = options.max_file_bytes](cc::string_view uri)
    { return impl::resolve_uri_from_filesystem(uri, max_bytes); };
}

cc::result<cc::pinned_data<byte const>> impl::resolve_uri_from_filesystem(cc::string_view uri, i64 max_bytes)
{
    auto const path = percent_decode(uri);

#ifdef CC_OS_WINDOWS
    // `NUL` opens, states a size of 0 and reads as empty, which no check below can tell from an empty file.
    if (names_windows_device(path))
        return cc::error(cc::format("shaped-viewer: '{}' names a device, not a regular file", path));
#endif

    // The adapter owns the buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return cc::error(cc::format("shaped-viewer: cannot open '{}'", path));

    auto stream = adapter.value().stream();

    // "Regular" inferred from the stream below stands in for a file-kind query; issue: feat-cc-file-kind-query
    // The size the source states, checked against the ceiling before a byte is read.
    auto const size = stream.size();
    if (size.has_error() || size.value() < 0)
        return cc::error(cc::format("shaped-viewer: '{}' is not a regular file", path));
    if (size.value() > max_bytes)
        return cc::error(
            cc::format("shaped-viewer: '{}' is {} bytes, over the {} a load reads", path, size.value(), max_bytes));

    auto bytes = cc::vector<byte>();
    bytes.resize_to_uninitialized(size.value());
    if (stream.read_exact(bytes).has_error())
        return cc::error(cc::format("shaped-viewer: cannot read '{}'", path));

    // A regular file ends where it said it would; a device that states 0 and keeps yielding does not.
    auto probe = byte(0);
    auto const more = stream.read(cc::span<byte>(&probe, 1));
    if (more.has_error() || more.value() != 0)
        return cc::error(cc::format("shaped-viewer: '{}' is not a regular file", path));

    // Moved into the pin rather than copied, so a large `.glb` is read once and never duplicated.
    return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bytes)));
}

bool impl::names_windows_device(cc::string_view path)
{
    // A path rule that belongs in clean-core; issue: feat-cc-file-kind-query
    if (path.size() >= 4 && is_separator(path[0]) && is_separator(path[1]) && path[2] == '.' && is_separator(path[3]))
        return true;

    auto const is_device = [](cc::string_view component)
    {
        auto end = isize(0);
        while (end < component.size() && component[end] != '.' && component[end] != ':')
            ++end;
        while (end > 0 && component[end - 1] == ' ')
            --end;
        auto upper = cc::string(component.subview({.offset = 0, .size = end}));
        auto* const c = upper.data();
        for (auto i = isize(0); i < upper.size(); ++i)
            c[i] = cc::to_upper(c[i]);
        auto const u = cc::string_view(upper);
        if (u == "CON" || u == "PRN" || u == "AUX" || u == "NUL" || u == "CONIN$" || u == "CONOUT$")
            return true;
        if (u.size() < 4 || !(u.starts_with("COM") || u.starts_with("LPT")))
            return false;
        auto const digit = u.subview(3);
        // 1-9, and the superscripts Windows also accepts there: U+00B9, U+00B2 and U+00B3 in UTF-8.
        return (digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') || digit == "\xC2\xB9" || digit == "\xC2\xB2"
            || digit == "\xC2\xB3";
    };

    auto start = isize(0);
    for (auto i = isize(0); i <= path.size(); ++i)
    {
        if (i < path.size() && !is_separator(path[i]))
            continue;
        if (is_device(path.subview({.offset = start, .size = i - start})))
            return true;
        start = i + 1;
    }
    return false;
}

cc::string impl::percent_decode(cc::string_view uri)
{
    auto out = cc::string();
    out.reserve_back(uri.size());

    for (auto i = isize(0); i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && cc::is_hex_digit(uri[i + 1]) && cc::is_hex_digit(uri[i + 2]))
        {
            out += char(hex_value(uri[i + 1]) * 16 + hex_value(uri[i + 2]));
            i += 2;
        }
        else
            out += uri[i];
    }
    return out;
}

cc::string_view impl::directory_of(cc::string_view uri)
{
    for (auto i = uri.size(); i > 0; --i)
        if (is_separator(uri[i - 1]))
            return uri.subview({.offset = 0, .size = i});
    return {};
}

cc::string impl::join_uri(cc::string_view base, cc::string_view relative)
{
    if (base.empty() || is_absolute(relative))
        return cc::string(relative);

    auto const dir = directory_of(base);
    if (dir.empty())
        return cc::string(relative);

    auto out = cc::string(dir);
    out += relative;
    return out;
}

cc::string impl::extension_of(cc::string_view uri)
{
    // A query or fragment is not part of the name, and `car.glb#mesh3` must still read as a glb.
    auto name = uri;
    for (auto i = isize(0); i < name.size(); ++i)
        if (name[i] == '?' || name[i] == '#')
        {
            name = name.subview({.offset = 0, .size = i});
            break;
        }

    for (auto i = name.size(); i > 0; --i)
    {
        if (is_separator(name[i - 1]))
            return {};
        if (name[i - 1] == '.')
        {
            auto out = cc::string(name.subview({.offset = i, .size = name.size() - i}));
            auto* const chars = out.data();
            for (auto k = isize(0); k < out.size(); ++k)
                chars[k] = cc::to_lower(chars[k]);
            return out;
        }
    }
    return {};
}
} // namespace sv
