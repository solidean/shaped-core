#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/from_string.hh>

using namespace cc::primitive_defines;

cc::optional<cc::string> cc::impl::read_text_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};

    auto bytes = adapter.value().stream().read_all();
    if (bytes.has_error())
        return {};

    auto const& data = bytes.value();
    return cc::string(cc::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
}

cc::optional<cc::string> cc::impl::read_trimmed_file(cc::string_view path)
{
    auto const text = cc::impl::read_text_file(path);
    if (!text.has_value())
        return {};

    auto const value = cc::impl::trimmed(text.value());
    if (value.empty())
        return {};
    return cc::string(value);
}

cc::optional<i64> cc::impl::read_int_file(cc::string_view path)
{
    auto const text = cc::impl::read_text_file(path);
    if (!text.has_value())
        return {};
    return cc::from_string<i64>(cc::impl::trimmed(text.value()));
}

cc::string_view cc::impl::trimmed(cc::string_view s)
{
    while (!s.empty() && cc::is_space(s.front()))
        s = s.subview(1);
    while (!s.empty() && cc::is_space(s.back()))
        s = s.subview_clamped(0, s.size() - 1);
    return s;
}

bool cc::impl::next_line(cc::string_view& rest, cc::string_view& out)
{
    if (rest.empty())
        return false;

    auto const nl = rest.find('\n');
    if (nl < 0)
    {
        out = rest;
        rest = {};
    }
    else
    {
        out = rest.subview_clamped(0, nl);
        rest = rest.subview(nl + 1);
    }
    return true;
}

cc::optional<cc::string> cc::impl::field_from(cc::string_view text, cc::string_view key, char separator)
{
    auto rest = text;
    auto line = cc::string_view();
    while (cc::impl::next_line(rest, line))
    {
        auto const at = line.find(separator);
        if (at < 0)
            continue;
        if (cc::impl::trimmed(line.subview_clamped(0, at)) != key)
            continue;

        auto value = cc::impl::trimmed(line.subview(at + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.subview_clamped(1, value.size() - 2);
        return cc::string(value);
    }
    return {};
}
