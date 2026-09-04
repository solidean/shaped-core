#include "endpoint.hh"

#include <clean-core/string/char_predicates.hh>

namespace cnet
{
namespace
{
cc::optional<i32> parse_port(cc::string_view text)
{
    if (text.empty() || text.size() > 5)
        return {};
    u32 value = 0;
    for (auto const c : text)
    {
        if (!cc::is_digit(c))
            return {};
        value = value * 10 + u32(c - '0');
    }
    if (value > 0xFFFF)
        return {};
    return i32(value);
}
} // namespace

cc::optional<endpoint> endpoint::parse(cc::string_view text)
{
    if (text.empty())
        return {};

    if (text[0] == '[')
    {
        auto const close = text.find(']');
        if (close < 0)
            return {};
        auto const rest = text.subview(close + 1);
        if (rest.empty() || rest[0] != ':')
            return {};

        auto const address = ip_address::parse(text.subview({.start = 1, .end = close}));
        if (!address.has_value() || address.value().family() != ip_family::v6)
            return {};
        auto const port = parse_port(rest.subview(1));
        if (!port.has_value())
            return {};
        return endpoint(address.value(), port.value());
    }

    // Unbracketed: the last colon separates the port, and an IPv6 address has more than one.
    auto const colon = text.rfind(':');
    if (colon < 0)
        return {};
    auto const host = text.subview({.offset = 0, .size = colon});
    if (host.contains(':'))
        return {}; // an unbracketed IPv6 address, which is exactly the ambiguity brackets exist to remove

    auto const address = ip_address::parse(host);
    if (!address.has_value())
        return {};
    auto const port = parse_port(text.subview(colon + 1));
    if (!port.has_value())
        return {};
    return endpoint(address.value(), port.value());
}

cc::string endpoint::to_string() const
{
    auto out = cc::string();
    auto const bracketed = address.family() == ip_family::v6;
    if (bracketed)
        out.push_back('[');
    out += address.to_string();
    if (bracketed)
        out.push_back(']');
    out.push_back(':');

    // Small enough that a hand-rolled decimal beats reaching for a formatter.
    char buf[5];
    isize n = 0;
    auto value = u32(port);
    do
    {
        buf[n++] = char('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (n > 0)
        out.push_back(buf[--n]);

    return out;
}
} // namespace cnet
