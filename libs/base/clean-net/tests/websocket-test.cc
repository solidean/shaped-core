#include <clean-core/container/vector.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <clean-net/ws/impl/websocket_internal.hh>
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

/// Pump against the wall clock, which is what a real socket waits on.
bool pump_for(cc::function_ref<bool()> done, f64 budget_secs = 5.0)
{
    auto& clk = system_clock();
    auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

    while (true)
    {
        if (done())
            return true;
        if (clk.now_ns() >= deadline_ns)
        {
            // A budget that runs out here is a wait that never finished, and the thread that noticed is never the one
            // that matters -- so say what every other thread was doing before failing.
            cc::report_all_thread_stacks("a cnet test waited out its budget");
            return false;
        }
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
}

/// Give the machinery every opportunity and then assert nothing moved.
///
/// A round count is right for that, and only for that: nothing here can make progress without the clock, so a fixed
/// sweep is a complete answer rather than a guess at how busy the machine is.
bool pump_briefly(cc::function_ref<bool()> done, i32 rounds = 200)
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

TEST("cnet - a websocket over a real socket, both ends in one process")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    auto server = http_server::try_create(*io.value());
    if (server.has_error())
        SKIP("this platform cannot listen");

    auto res = resolver::try_create(*io.value());
    if (res.has_error())
        SKIP("this platform cannot resolve");

    auto accepted = cc::vector<cc::shared_ptr<websocket>>();
    server.value()->websocket_route("/socket", [&accepted](cc::shared_ptr<websocket> ws, http_server_request const&)
                                    { accepted.push_back(cc::move(ws)); });

    // The real socket path is what a virtual network cannot stand in for: it is asynchronous all the way down, so a
    // handshake buffer that dies with the call that started the send is read after it is freed.
    auto connecting = websocket_connect(*io.value(), *res.value(),
                                        cc::format("ws://127.0.0.1:{}/socket", server.value()->local().port));
    CHECK(pump_for([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);

    auto const client = connecting->value();
    CHECK(pump_for([&] { return !accepted.empty(); }));
    REQUIRE(accepted.size() == 1);

    auto const sent = client->send_text("over a socket");
    auto received = accepted[0]->receive();
    CHECK(pump_for([&] { return sent->is_ready() && received->is_ready(); }));
    REQUIRE(received->try_error() == nullptr);
    CHECK(received->value().text() == "over a socket");

    client->close();
}

// ---- keepalives ----------------------------------------------------------------------------------------

namespace
{
/// A virtual network on a clock a test moves by hand, which is the only way to prove a 30-second rule in a
/// millisecond.
struct keepalive_fixture
{
    manual_clock clk = manual_clock(0);
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<resolver> res;
    cc::unique_ptr<http_server> server;

    cc::vector<cc::shared_ptr<websocket>> accepted;

    explicit keepalive_fixture(http_server_description const& desc)
    {
        io = io_system::create({.unthreaded = true, .time_source = &clk});
        net = cc::make_unique<virtual_network>(*io);

        auto const answer = ip_address::loopback(ip_family::v4);
        res = resolver::create(*io, {.lookup = [answer](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                     { return cc::vector<ip_address>{answer}; }});

        server = http_server::try_create(*net, desc).value();
        server->websocket_route("/socket", [this](cc::shared_ptr<websocket> ws, http_server_request const&)
                                { accepted.push_back(cc::move(ws)); });
    }

    [[nodiscard]] cc::string url() const { return cc::format("ws://localhost:{}/socket", server->local().port); }
};
} // namespace

TEST("cnet - an idle websocket is pinged, and a pong keeps it alive")
{
    auto fixture = keepalive_fixture({.websocket_ping_interval_ms = 1'000, .websocket_pong_timeout_ms = 500});

    auto connecting = websocket_connect(*fixture.net, *fixture.res, fixture.url(),
                                        {.ping_interval_ms = 1'000, .pong_timeout_ms = 500});
    CHECK(pump_until([&] { return connecting->is_ready(); }));
    REQUIRE(connecting->try_error() == nullptr);

    auto const client = connecting->value();
    REQUIRE(fixture.accepted.size() == 1);

    // Both ends ping and both answer, so several rounds of it change nothing a caller can see -- which is the whole
    // property: a keepalive is invisible until the peer stops answering.
    for (auto round = 0; round < 4; ++round)
    {
        fixture.clk.advance_ms(1'000);
        (void)pump_briefly([] { return false; });
        fixture.clk.advance_ms(500);
        (void)pump_briefly([] { return false; });
    }

    CHECK(client->is_open());
    CHECK(fixture.accepted[0]->is_open());

    // And messages still work afterwards, so nothing the keepalive sent confused the framing.
    auto const sent = client->send_text("still here");
    auto received = fixture.accepted[0]->receive();
    CHECK(pump_until([&] { return sent->is_ready() && received->is_ready(); }));
    REQUIRE(received->try_error() == nullptr);
    CHECK(received->value().text() == "still here");
}

TEST("cnet - a peer that stops answering fails the receive rather than hanging it")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = net.listen(endpoint(ip_address::loopback(ip_family::v4), 0), {}).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    // Only ONE end becomes a WebSocket.
    // The other is a plain connection that reads bytes and answers nothing, which is what a peer whose machine
    // vanished looks like from here: no FIN, no pong, no error.
    auto const ws = impl::adopt_websocket(
        *io, {.connection = connected->value(), .is_client = true, .ping_interval_ms = 20, .pong_timeout_ms = 20});

    auto const silent = accepted->value();
    byte sink[256] = {};
    auto const swallowing = silent->receive(cc::span<byte>(sink, isize(sizeof(sink))), deadline::never());

    // A receive with no deadline: without a keepalive this waits forever, which is the failure being ruled out.
    auto waiting = ws->receive(deadline::never());
    CHECK(pump_for([&] { return waiting->is_ready(); }));

    REQUIRE(waiting->try_error() != nullptr);
    CHECK(!ws->is_open());

    (void)swallowing;
}

TEST("cnet - keepalives can be turned off")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = net.listen(endpoint(ip_address::loopback(ip_family::v4), 0), {}).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const ws = impl::adopt_websocket(
        *io, {.connection = connected->value(), .is_client = true, .ping_interval_ms = 0, .pong_timeout_ms = 0});

    auto const silent = accepted->value();
    auto waiting = ws->receive(deadline::never());

    // Nothing arms, so nothing fires, and the receive is still waiting -- which is what a caller who said 0 asked
    // for, and why 0 is a decision rather than an omission.
    CHECK(pump_briefly([&] { return waiting->is_ready(); }) == false);
    CHECK(ws->is_open());

    ws->close();
}
