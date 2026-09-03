#include "websocket.hh"

#include <clean-core/string/format.hh>
#include <clean-net/http/http_target.hh>
#include <clean-net/http/impl/http1.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/transport/connect.hh>
#include <clean-net/ws/impl/websocket_internal.hh>
#include <clean-net/ws/impl/ws_frame.hh>

// The client handshake: an HTTP request that, if it works, stops being HTTP.
//
// WHY THIS IS NOT THE HTTP CLIENT.
// The upgrade needs the connection afterwards, and everything the http_client does -- pooling, redirects, retries --
// is built on being finished with a connection when a response ends.
// So this speaks the one request it needs directly, and the parser it uses is the same one.

namespace cnet
{
namespace
{
constexpr isize k_handshake_read_chunk = 4 * 1024;

/// The state one handshake carries while it is in flight.
struct handshake
{
    cc::shared_async<cc::shared_ptr<websocket>> promise;
    cc::shared_ptr<stream_connection> connection;
    cc::string expected_accept;
    websocket_options options;
    deadline d;
    cancel_token token;

    impl::http1_parser parser;
    cc::vector<byte> buffer;
    cc::vector<byte> pending;
};

void fail(cc::shared_ptr<handshake> const& h, error e)
{
    if (h->connection.is_valid())
        h->connection->close();
    h->promise->push_error(to_async_error(cc::move(e)));
}

[[nodiscard]] error protocol(cc::string message)
{
    return {.code = error_code::protocol_error, .native_code = 0, .message = cc::move(message)};
}

/// Check the 101 and everything that has to come with it.
[[nodiscard]] cc::result<cc::string, error> check_response(handshake const& h)
{
    auto const& head = h.parser.response();

    // A server that does not understand the upgrade answers with an ordinary status, and that is a clearer failure
    // than anything further down would be.
    if (head.status != 101)
        return cc::error(protocol(cc::format("the server answered {} rather than upgrading to a websocket", head.status)));

    auto const upgrade = head.headers.get("Upgrade");
    if (!upgrade.has_value() || !header_names_equal(upgrade.value(), "websocket"))
        return cc::error(protocol("a 101 without an Upgrade: websocket"));

    // The Connection header is a comma-separated list in general, but on a 101 it is this one token and a server
    // sending anything else is one this handshake did not reach.
    auto const connection = head.headers.get("Connection");
    if (!connection.has_value() || !header_names_equal(connection.value(), "Upgrade"))
        return cc::error(protocol("a 101 without a Connection: Upgrade"));

    // This is what makes the 101 an answer to THIS request rather than a cached or replayed one.
    auto const accept = head.headers.get("Sec-WebSocket-Accept");
    if (!accept.has_value() || accept.value() != h.expected_accept)
        return cc::error(protocol("a 101 whose Sec-WebSocket-Accept does not match the key that was sent"));

    auto const chosen = head.headers.get("Sec-WebSocket-Protocol");
    if (!chosen.has_value())
        return cc::string();

    // A server may only pick from what was offered; anything else is a subprotocol the caller has no code for.
    for (auto const& offered : h.options.protocols)
        if (offered == chosen.value())
            return cc::string(chosen.value());

    return cc::error(protocol("the server picked a subprotocol that was never offered"));
}

void read_response(cc::shared_ptr<handshake> const& h);

void consume(cc::shared_ptr<handshake> const& h)
{
    // The head is all this reads; the body of a 101 does not exist, and every byte after it belongs to the WebSocket.
    while (!h->pending.empty() && !h->parser.head_complete())
    {
        auto fed = h->parser.feed(cc::span<byte const>(h->pending.data(), h->pending.size()),
                                  [](cc::span<byte const>) { return isize(0); });
        if (fed.has_error())
        {
            fail(h, cc::move(fed).error());
            return;
        }

        auto const taken = fed.value();
        if (taken > 0)
        {
            auto rest = cc::vector<byte>();
            for (auto i = taken; i < h->pending.size(); ++i)
                rest.push_back(h->pending[i]);
            h->pending = cc::move(rest);
        }

        if (taken == 0 && !h->parser.head_complete())
            break; // nothing more can be made of what is here
    }

    if (!h->parser.head_complete())
    {
        read_response(h);
        return;
    }

    auto checked = check_response(*h);
    if (checked.has_error())
    {
        fail(h, cc::move(checked).error());
        return;
    }

    auto ws = impl::adopt_websocket(cc::move(h->connection), true, cc::move(checked).value(), cc::move(h->pending),
                                    h->options.max_message_bytes, h->token);
    h->promise->push_value(cc::move(ws));
}

void read_response(cc::shared_ptr<handshake> const& h)
{
    auto buffer = cc::span<byte>(h->buffer.data(), h->buffer.size());
    impl::when_ready(h->connection->receive(buffer, h->d, h->token),
                     [h](cc::shared_async<isize> const& received)
                     {
                         if (received->has_error())
                         {
                             h->connection->close();
                             h->promise->push_error(received->propagate_error());
                             return;
                         }

                         auto const n = received->value();
                         if (n == 0)
                         {
                             fail(h, protocol("the server closed the connection during the websocket handshake"));
                             return;
                         }

                         for (isize i = 0; i < n; ++i)
                             h->pending.push_back(h->buffer[i]);

                         consume(h);
                     });
}

/// The request that asks for the upgrade.
[[nodiscard]] cc::string build_request(http_target const& target, cc::string_view key, websocket_options const& options)
{
    auto out = cc::string();
    out += "GET ";
    out += target.request_target();
    out += " HTTP/1.1\r\nHost: ";
    out += target.host_header();
    out += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    out += key;
    out += "\r\nSec-WebSocket-Version: 13\r\n";

    if (!options.protocols.empty())
    {
        out += "Sec-WebSocket-Protocol: ";
        for (isize i = 0; i < options.protocols.size(); ++i)
        {
            if (i > 0)
                out += ", ";
            out += options.protocols[i];
        }
        out += "\r\n";
    }

    out += "\r\n";
    return out;
}

/// The `ws`/`wss` URL as the thing `http_target` already knows how to check.
///
/// The two schemes differ from `http`/`https` in name only -- same default ports, same authority rules -- so this
/// rewrites rather than duplicating the parsing and its refusals.
[[nodiscard]] cc::result<http_target, error> parse_ws_url(cc::string_view url)
{
    auto const secure = url.starts_with("wss://") || url.starts_with("WSS://");
    auto const plain = url.starts_with("ws://") || url.starts_with("WS://");

    if (!secure && !plain)
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::format("'{}' is not a ws:// or wss:// url", url)});

    auto rewritten = cc::string(secure ? "https" : "http");
    rewritten += url.subview(secure ? 3 : 2);

    auto target = http_target::parse(rewritten);
    if (target.has_error())
        return cc::error(cc::move(target).error());

    return cc::move(target).value();
}

/// Everything after the connect, which is the same however the connection was obtained.
void upgrade(cc::shared_ptr<handshake> const& h,
             cc::shared_async<cc::shared_ptr<stream_connection>> connecting,
             cc::string host,
             bool secure,
             cc::string request)
{
    impl::when_ready(
        cc::move(connecting),
        [h, host = cc::move(host), secure,
         request = cc::move(request)](cc::shared_async<cc::shared_ptr<stream_connection>> const& connected)
        {
            if (connected->has_error())
            {
                h->promise->push_error(connected->propagate_error());
                return;
            }

            auto const send_upgrade = [h, request](cc::shared_ptr<stream_connection> connection)
            {
                h->connection = cc::move(connection);

                auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(request.data()), request.size());

                impl::when_ready(h->connection->send(bytes, h->d, h->token),
                                 [h](cc::shared_async<cc::unit> const& sent)
                                 {
                                     if (sent->has_error())
                                     {
                                         h->connection->close();
                                         h->promise->push_error(sent->propagate_error());
                                         return;
                                     }
                                     read_response(h);
                                 });
            };

            if (!secure)
            {
                send_upgrade(connected->take_value());
                return;
            }

            // The NAME, not the address: the certificate is checked against what the caller asked for.
            impl::when_ready(tls_connect(connected->take_value(), host, h->options.tls, h->d, h->token),
                             [h, send_upgrade](cc::shared_async<cc::shared_ptr<stream_connection>> const& secured)
                             {
                                 if (secured->has_error())
                                 {
                                     h->promise->push_error(secured->propagate_error());
                                     return;
                                 }
                                 send_upgrade(secured->take_value());
                             });
        });
}

/// Everything before the connect, which is the same too.
///
/// Returns the handshake to drive, or the failure that means there is nothing to connect to.
[[nodiscard]] cc::result<cc::shared_ptr<handshake>, error> prepare(cc::string_view url,
                                                                   websocket_options const& options,
                                                                   deadline d,
                                                                   cancel_token const& token,
                                                                   http_target& target,
                                                                   cc::string& request)
{
    auto parsed = parse_ws_url(url);
    if (parsed.has_error())
        return cc::error(cc::move(parsed).error());
    target = cc::move(parsed).value();

    auto key = impl::generate_websocket_key();
    if (key.has_error())
        return cc::error(cc::move(key).error());

    auto accept = impl::websocket_accept_key(key.value());
    if (accept.has_error())
        return cc::error(cc::move(accept).error());

    auto h = cc::make_shared<handshake>();
    h->promise = cc::make_async_manual<cc::shared_ptr<websocket>>();
    h->expected_accept = cc::move(accept).value();
    h->options = options;
    h->d = d;
    h->token = token;
    h->parser.start_response(http_method::get);
    h->buffer.resize_to_defaulted(k_handshake_read_chunk);

    request = build_request(target, key.value(), options);
    return h;
}
} // namespace

cc::shared_async<cc::shared_ptr<websocket>> websocket_connect(transport& t,
                                                              resolver& r,
                                                              cc::string_view url,
                                                              websocket_options const& options,
                                                              deadline d,
                                                              cancel_token const& token)
{
    auto target = http_target();
    auto request = cc::string();

    auto prepared = prepare(url, options, d, token, target, request);
    if (prepared.has_error())
        return impl::failed_async<cc::shared_ptr<websocket>>(cc::move(prepared).error());

    auto const h = cc::move(prepared).value();
    upgrade(h, connect_to_host(t, r, target.host, target.port, {.timeout = d}, token), target.host, target.secure,
            cc::move(request));
    return h->promise;
}

cc::shared_async<cc::shared_ptr<websocket>> websocket_connect(io_system& io,
                                                              resolver& r,
                                                              cc::string_view url,
                                                              websocket_options const& options,
                                                              deadline d,
                                                              cancel_token const& token)
{
    auto target = http_target();
    auto request = cc::string();

    auto prepared = prepare(url, options, d, token, target, request);
    if (prepared.has_error())
        return impl::failed_async<cc::shared_ptr<websocket>>(cc::move(prepared).error());

    auto const h = cc::move(prepared).value();
    upgrade(h, connect_to_host(io, r, target.host, target.port, {.timeout = d}, token), target.host, target.secure,
            cc::move(request));
    return h->promise;
}
} // namespace cnet
