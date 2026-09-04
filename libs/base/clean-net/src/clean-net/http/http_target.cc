#include "http_target.hh"

#include <clean-core/string/format.hh>

namespace cnet
{
namespace
{
[[nodiscard]] error refused(cc::string_view why, cc::string_view what)
{
    return {.code = error_code::invalid_argument, .native_code = 0, .message = cc::format("{}: {}", why, what)};
}
} // namespace

cc::result<http_target, error> http_target::parse(cc::string_view text)
{
    auto parsed = cc::uri::parse(text);
    if (!parsed.has_value())
        return cc::error(refused("not a URL", text));

    return from_uri(cc::move(parsed.value()));
}

cc::result<http_target, error> http_target::from_uri(cc::uri url)
{
    auto const view = url.view();

    if (!view.is_absolute())
        return cc::error(refused("a relative URL has nothing to connect to", view.text()));

    auto const scheme = view.scheme().to_lower_ascii();
    if (scheme != "http" && scheme != "https")
        return cc::error(refused("not an http or https URL", view.text()));

    if (!view.has_authority())
        return cc::error(refused("no host", view.text()));

    // Refused rather than ignored: dropping the credentials silently changes which host is contacted, and reading
    // that URL as pointing at the FIRST host is the mistake the trick relies on.
    if (view.has_userinfo())
        return cc::error(refused("credentials in a URL are not accepted", view.text()));

    auto host_text = view.host();
    if (host_text.size() >= 2 && host_text[0] == '[' && host_text[host_text.size() - 1] == ']')
        host_text = host_text.subview({.offset = 1, .size = host_text.size() - 2});

    if (host_text.empty())
        return cc::error(refused("no host", view.text()));

    // A percent-escape in a host is legal RFC 3986 and means nothing here: a name is resolved as written, and an
    // address literal has no escapes.
    // Refusing is better than resolving something the author did not write.
    if (host_text.contains('%'))
        return cc::error(refused("a percent-escape in the host is not accepted", view.text()));

    auto target = http_target();
    target.host = host_text.to_lower_ascii();
    target.secure = scheme == "https";

    if (view.has_port())
    {
        // `has_port` and `port` say different things: a `:` was there, and it held a number that fits.
        // A port too large to represent comes back absent, and reading that as "no port given" would send the
        // request to 443 instead of refusing a URL nobody can honour.
        auto const explicit_port = view.port();
        if (!explicit_port.has_value() || explicit_port.value() <= 0 || explicit_port.value() > 0xFFFF)
            return cc::error(refused("the port is not a usable one", view.text()));

        target.port = explicit_port.value();
    }
    else
    {
        target.port = target.secure ? 443 : 80;
    }

    target.url = cc::move(url);
    return target;
}

cc::string http_target::request_target() const
{
    auto const view = url.view();

    auto const path = view.path().empty() ? cc::string_view("/") : view.path();
    if (!view.has_query())
        return cc::string(path);

    return cc::format("{}?{}", path, view.query());
}

cc::string http_target::origin() const
{
    auto const scheme = url.view().scheme().to_lower_ascii();
    auto const bracketed = host.contains(':') ? cc::format("[{}]", host) : host;

    auto const default_port = secure ? 443 : 80;
    if (port == default_port)
        return cc::format("{}://{}", scheme, bracketed);
    return cc::format("{}://{}:{}", scheme, bracketed, port);
}

cc::string http_target::host_header() const
{
    auto const bracketed = host.contains(':') ? cc::format("[{}]", host) : host;

    // The default port is left out, which is what every server and cache expects to see.
    auto const default_port = secure ? 443 : 80;
    if (port == default_port)
        return bracketed;
    return cc::format("{}:{}", bracketed, port);
}
} // namespace cnet
