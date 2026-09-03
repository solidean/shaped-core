#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <clean-net/ws/impl/ws_frame.hh>
#include <clean-net/ws/websocket.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Both ends of a real WebSocket, over a network that does not exist.
//
// The handshake, the framing, the masking rule and the control frames are all the ones that would go over a socket --
// only the socket is missing, which is why none of this needs a port.

namespace
{
bool pump_until(cc::function_ref<bool()> done, i32 rounds = 20000)
{
    for (i32 i = 0; i < rounds; ++i)
    {
        if (done())
            return true;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
    return done();
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view text)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
}

/// A virtual network with a server on it, ready to be given routes.
struct ws_fixture
{
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<resolver> res;
    cc::unique_ptr<http_server> server;

    /// Every WebSocket the server accepted, kept alive because nothing else holds one.
    cc::vector<cc::shared_ptr<websocket>> accepted;

    ws_fixture()
    {
        io = io_system::create({.unthreaded = true});
        net = cc::make_unique<virtual_network>(*io);

        auto const answer = ip_address::loopback(ip_family::v4);
        res = resolver::create(*io, {.lookup = [answer](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                     { return cc::vector<ip_address>{answer}; }});

        server = http_server::try_create(*net).value();
    }

    [[nodiscard]] cc::string url_for(cc::string_view path) const
    {
        return cc::format("ws://localhost:{}{}", server->local().port, path);
    }
};
} // namespace

TEST("cnet - a websocket carries a message each way")
{
    auto fixture = ws_fixture();

    fixture.server->websocket_route("/socket", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url_for("/socket"));
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);

    auto const client = connecting->value();
    CHECK(client->is_open());
    CHECK(fixture.accepted.size() == 1);

    // Client to server.
    auto const sent = client->send_text("ping from the client");
    auto const server_side = fixture.accepted[0];
    auto received = server_side->receive();

    CHECK(pump_until([&] { return sent->is_ready() && received->is_ready(); }));
    REQUIRE(received->try_error() == nullptr);
    CHECK(received->value().is_text);
    CHECK(received->value().text() == "ping from the client");

    // And back.
    auto const answered = server_side->send_text("pong from the server");
    auto back = client->receive();

    CHECK(pump_until([&] { return answered->is_ready() && back->is_ready(); }));
    REQUIRE(back->try_error() == nullptr);
    CHECK(back->value().text() == "pong from the server");
}

TEST("cnet - a websocket carries binary and large messages")
{
    auto fixture = ws_fixture();

    fixture.server->websocket_route("/socket", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url_for("/socket"));
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);
    auto const client = connecting->value();

    // Past 125 bytes and past 65535, which are the two points where the length encoding changes shape.
    auto payload = cc::vector<byte>();
    payload.resize_to_defaulted(70000);
    for (isize i = 0; i < payload.size(); ++i)
        payload[i] = byte(u8(i * 31 + 7));

    auto const sent = client->send_binary(payload);
    auto received = fixture.accepted[0]->receive();

    CHECK(pump_until([&] { return sent->is_ready() && received->is_ready(); }));
    REQUIRE(received->try_error() == nullptr);
    CHECK(!received->value().is_text);
    REQUIRE(received->value().data.size() == payload.size());

    auto same = true;
    for (isize i = 0; i < payload.size(); ++i)
        same = same && received->value().data[i] == payload[i];
    CHECK(same);
}

TEST("cnet - a websocket message that arrived before anybody asked is not lost")
{
    auto fixture = ws_fixture();

    fixture.server->websocket_route("/socket", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url_for("/socket"));
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);
    auto const client = connecting->value();

    // Two messages sent while nothing is receiving; both must still be there afterwards, in order.
    auto const first = client->send_text("one");
    auto const second = client->send_text("two");
    CHECK(pump_until([&] { return first->is_ready() && second->is_ready(); }));

    auto const server_side = fixture.accepted[0];

    auto a = server_side->receive();
    CHECK(pump_until([&] { return a->is_ready(); }));
    REQUIRE(a->try_error() == nullptr);
    CHECK(a->value().text() == "one");

    auto b = server_side->receive();
    CHECK(pump_until([&] { return b->is_ready(); }));
    REQUIRE(b->try_error() == nullptr);
    CHECK(b->value().text() == "two");
}

TEST("cnet - closing a websocket ends the other end's receive")
{
    auto fixture = ws_fixture();

    fixture.server->websocket_route("/socket", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url_for("/socket"));
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);
    auto const client = connecting->value();

    auto const server_side = fixture.accepted[0];
    auto waiting = server_side->receive();

    client->close();

    CHECK(pump_until([&] { return waiting->is_ready(); }));
    REQUIRE(waiting->try_error() != nullptr);
    CHECK(!server_side->is_open());
}

TEST("cnet - a request that is not an upgrade gets a 400 from a websocket route")
{
    auto fixture = ws_fixture();

    fixture.server->websocket_route("/socket", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto client = native_http_client(*fixture.net, *fixture.res);
    auto response = http_get(client, cc::format("http://localhost:{}/socket", fixture.server->local().port));

    CHECK(pump_until([&] { return response->is_ready(); }));
    REQUIRE(response->try_error() == nullptr);

    // A 400 rather than a 404: the path exists, and a client that meant to upgrade learns more from that.
    CHECK(response->value().status() == 400);
    CHECK(fixture.accepted.empty());
}

TEST("cnet - a websocket route and an ordinary route can share a path")
{
    auto fixture = ws_fixture();

    fixture.server->route(http_method::get, "/thing",
                          [](http_server_request const&) { return http_server_response::text("plain"); });
    fixture.server->websocket_route("/thing", [&fixture](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { fixture.accepted.push_back(cc::move(ws)); });

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url_for("/thing"));
    CHECK(pump_until([&] { return connecting->is_ready(); }));

    // The upgrade wins when the request asks for one, and the route is untouched otherwise -- which cannot be checked
    // here, because a plain GET on this path now answers 400 by the rule above.
    CHECK(connecting->try_error() == nullptr);
    CHECK(fixture.accepted.size() == 1);
}

TEST("cnet - websocket_connect refuses a url that is not ws")
{
    auto fixture = ws_fixture();

    auto connecting = websocket_connect(*fixture.net, *fixture.res, "http://localhost/socket");
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() != nullptr);
}

// ---- framing -------------------------------------------------------------------------------------------

TEST("cnet - the frame reader waits for a whole header")
{
    auto out = cc::vector<byte>();
    u8 const mask[4] = {0x11, 0x22, 0x33, 0x44};
    impl::write_frame(out, impl::ws_opcode::text, bytes_of("hello"), true, mask);

    // Every prefix short of the whole header is incomplete rather than an error.
    for (isize n = 0; n < 2 + 4; ++n)
    {
        auto const partial = impl::read_frame_header(cc::span<byte const>(out.data(), n));
        REQUIRE(partial.has_value());
        CHECK(!partial.value().has_value());
    }

    auto const header = impl::read_frame_header(out);
    REQUIRE(header.has_value());
    REQUIRE(header.value().has_value());
    CHECK(header.value().value().opcode == impl::ws_opcode::text);
    CHECK(header.value().value().masked);
    CHECK(header.value().value().fin);
    CHECK(header.value().value().payload_length == 5);
    CHECK(header.value().value().header_size == 6);
}

TEST("cnet - the frame reader refuses what the protocol forbids")
{
    // A reserved bit set.
    {
        byte const frame[] = {byte(0xC1), byte(0x00)};
        CHECK(impl::read_frame_header(frame).has_error());
    }

    // An opcode nobody defined.
    {
        byte const frame[] = {byte(0x83), byte(0x00)};
        CHECK(impl::read_frame_header(frame).has_error());
    }

    // A fragmented control frame.
    {
        byte const frame[] = {byte(0x09), byte(0x00)};
        CHECK(impl::read_frame_header(frame).has_error());
    }

    // A control frame past 125 bytes.
    {
        byte const frame[] = {byte(0x89), byte(0x7E), byte(0x01), byte(0x00)};
        CHECK(impl::read_frame_header(frame).has_error());
    }

    // A length written in more bytes than it needed.
    {
        byte const frame[] = {byte(0x81), byte(0x7E), byte(0x00), byte(0x05)};
        CHECK(impl::read_frame_header(frame).has_error());
    }
}

TEST("cnet - unmasking undoes masking, chunk by chunk")
{
    auto out = cc::vector<byte>();
    u8 const mask[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    impl::write_frame(out, impl::ws_opcode::binary, bytes_of("the quick brown fox"), true, mask);

    auto const header = impl::read_frame_header(out).value().value();

    auto payload = cc::vector<byte>();
    for (auto i = header.header_size; i < out.size(); ++i)
        payload.push_back(out[i]);

    // Split at a point that is not a multiple of four, which is the whole reason the offset exists.
    auto const split = isize(7);
    impl::unmask(cc::span<byte>(payload.data(), split), header.mask, 0);
    impl::unmask(cc::span<byte>(payload.data() + split, payload.size() - split), header.mask, split);

    CHECK(cc::string_view(reinterpret_cast<char const*>(payload.data()), payload.size()) == "the quick brown fox");
}

TEST("cnet - the accept key is the one RFC 6455 gives as an example")
{
    auto const accept = impl::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    REQUIRE(accept.has_value());
    CHECK(accept.value() == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST("cnet - a generated key is 16 bytes of base64 and never the same twice")
{
    auto const a = impl::generate_websocket_key();
    auto const b = impl::generate_websocket_key();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    CHECK(a.value().size() == 24);
    CHECK(a.value() != b.value());
}

TEST("cnet - close codes with holes in them")
{
    CHECK(impl::is_valid_close_code(1000));
    CHECK(impl::is_valid_close_code(1011));
    CHECK(impl::is_valid_close_code(4999));

    // Never assigned, and the two that exist only to be reported locally.
    CHECK(!impl::is_valid_close_code(1004));
    CHECK(!impl::is_valid_close_code(1005));
    CHECK(!impl::is_valid_close_code(1006));
    CHECK(!impl::is_valid_close_code(999));
    CHECK(!impl::is_valid_close_code(5000));
}
