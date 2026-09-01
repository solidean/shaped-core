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

/// The value one hex digit names; `cc` has `is_hex_digit` but no nibble decoder yet.
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

cc::result<cc::pinned_data<byte const>> impl::resolve_uri_from_filesystem(cc::string_view uri)
{
    auto const path = percent_decode(uri);

    // The adapter owns the buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return cc::error(cc::format("shaped-viewer: cannot open '{}'", path));

    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return cc::error(cc::format("shaped-viewer: cannot read '{}'", path));

    // Moved into the pin rather than copied, so a large `.glb` is read once and never duplicated.
    return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bytes.value())));
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
