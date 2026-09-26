#include "framing.hh"

#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

using namespace cc::primitive_defines;

namespace
{
/// `Content-Length` in any case, as HTTP header names are.
[[nodiscard]] bool is_content_length(cc::string_view name)
{
    constexpr auto expected = cc::string_view("content-length");
    if (name.size() != expected.size())
        return false;
    for (auto i = isize(0); i < name.size(); ++i)
    {
        auto c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != expected[i])
            return false;
    }
    return true;
}

/// The body length a header block names, or -1 when it names none or names it badly.
[[nodiscard]] i64 content_length_of(cc::string_view headers)
{
    auto length = i64(-1);
    while (!headers.empty())
    {
        auto end = headers.find("\r\n");
        auto const line = end < 0 ? headers : headers.subview({.offset = 0, .size = end});
        headers.remove_prefix(end < 0 ? headers.size() : end + 2);

        auto const colon = line.find(':');
        if (colon < 0)
            continue;
        if (!is_content_length(line.subview({.offset = 0, .size = colon})))
            continue;
        auto value = line.subview(colon + 1);
        while (!value.empty() && value[0] == ' ')
            value.remove_prefix(1);
        while (!value.empty() && value[value.size() - 1] == ' ')
            value.remove_suffix(1);
        auto parsed = cc::from_string<i64>(value);
        length = parsed.has_value() && parsed.value() >= 0 ? parsed.value() : -1;
    }
    return length;
}
} // namespace

cc::vector<cc::string> lsp::frame_reader::feed(cc::span<byte const> bytes)
{
    auto out = cc::vector<cc::string>();
    if (_is_broken)
        return out;
    _buffer.append(cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));

    auto consumed = isize(0);
    while (true)
    {
        auto const rest = cc::string_view(_buffer).subview(consumed);
        auto const header_end = rest.find("\r\n\r\n");
        if (header_end < 0)
            break;
        auto const length = content_length_of(rest.subview({.offset = 0, .size = header_end}));
        if (length < 0)
        {
            CC_LOG_ERROR("a message header names no valid Content-Length; the stream has lost its framing");
            _is_broken = true;
            _buffer.clear();
            return out;
        }
        auto const body_start = header_end + 4;
        if (rest.size() - body_start < length)
            break;
        out.push_back(cc::string(rest.subview({.offset = body_start, .size = isize(length)})));
        consumed += body_start + isize(length);
    }
    if (consumed > 0)
        _buffer = _buffer.substring(consumed);
    return out;
}

cc::string lsp::frame(cc::string_view body)
{
    auto out = cc::format("Content-Length: {}\r\n\r\n", body.size());
    out.append(body);
    return out;
}
