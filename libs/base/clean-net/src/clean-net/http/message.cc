#include "message.hh"

namespace cnet
{
namespace
{
[[nodiscard]] char lowered(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}
} // namespace

cc::string_view to_string(http_method method)
{
    switch (method)
    {
    case http_method::get:
        return "GET";
    case http_method::head:
        return "HEAD";
    case http_method::post:
        return "POST";
    case http_method::put:
        return "PUT";
    case http_method::del:
        return "DELETE";
    case http_method::patch:
        return "PATCH";
    case http_method::options:
        return "OPTIONS";
    }
    return "GET";
}

bool is_idempotent(http_method method)
{
    switch (method)
    {
    case http_method::get:
    case http_method::head:
    case http_method::put:
    case http_method::del:
    case http_method::options:
        return true;

    // POST is an order and PATCH is a change to apply; sending either twice is not what the caller asked for.
    case http_method::post:
    case http_method::patch:
        return false;
    }
    return false;
}

bool header_names_equal(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (lowered(a[i]) != lowered(b[i]))
            return false;
    return true;
}

void http_headers::add(cc::string_view name, cc::string_view value)
{
    _entries.push_back({.name = cc::string(name), .value = cc::string(value)});
}

void http_headers::set(cc::string_view name, cc::string_view value)
{
    remove(name);
    add(name, value);
}

void http_headers::set_if_absent(cc::string_view name, cc::string_view value)
{
    if (!contains(name))
        add(name, value);
}

void http_headers::remove(cc::string_view name)
{
    auto kept = cc::vector<http_header>();
    for (auto& entry : _entries)
        if (!header_names_equal(entry.name, name))
            kept.push_back(cc::move(entry));
    _entries = cc::move(kept);
}

bool http_headers::contains(cc::string_view name) const
{
    return get(name).has_value();
}

cc::optional<cc::string_view> http_headers::get(cc::string_view name) const
{
    for (auto const& entry : _entries)
        if (header_names_equal(entry.name, name))
            return cc::string_view(entry.value);
    return {};
}

cc::vector<cc::string_view> http_headers::get_all(cc::string_view name) const
{
    auto values = cc::vector<cc::string_view>();
    for (auto const& entry : _entries)
        if (header_names_equal(entry.name, name))
            values.push_back(cc::string_view(entry.value));
    return values;
}

cc::optional<i64> http_response_head::content_length() const
{
    auto const value = headers.get("Content-Length");
    if (!value.has_value())
        return {};

    auto length = i64(0);
    auto digits = 0;
    for (auto const c : value.value())
    {
        if (c < '0' || c > '9')
            return {};

        length = length * 10 + (c - '0');
        ++digits;

        // A length longer than any body anybody can hold is a malformed header rather than a big file.
        if (digits > 18)
            return {};
    }

    if (digits == 0)
        return {};
    return length;
}
} // namespace cnet
