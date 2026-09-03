#include <clean-core/container/vector.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/stream.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// TCP as a caller meets it: connect, accept, send, receive, all as cc::shared_async.
// Every test runs unthreaded, which makes the ordering deterministic and the whole thing debuggable on one thread --
// and it is also the mode wasm and a threads-off build always get.
// Everything is loopback, so nothing leaves the machine.

namespace
{
/// Drive the process pump until `done`, or give up.
///
/// The budget is wall-clock rather than a spin count: a round count measures how fast this machine is rather than
/// how long the reactor was given, and a debug build burns thousands of yields in the time one handoff takes.
bool wait_for(cc::function_ref<bool()> done, f64 budget_secs = 5.0)
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

/// Wait for one async handle and hand back whether it succeeded.
template <class T>
[[nodiscard]] bool settled_ok(cc::shared_async<T> const& handle)
{
    if (!wait_for([&] { return handle->is_ready(); }))
        return false;
    return handle->try_error() == nullptr;
}

/// A listener on loopback, with the io_system it belongs to.
struct server_fixture
{
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<stream_listener> listener;

    [[nodiscard]] bool up() const { return io.is_valid() && listener.is_valid(); }
    [[nodiscard]] endpoint where() const { return listener->local(); }
};

[[nodiscard]] server_fixture make_server()
{
    auto fixture = server_fixture();

    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        return fixture;
    fixture.io = cc::move(io).value();

    auto listener = stream_listener::try_create(*fixture.io, endpoint(ip_address::loopback(ip_family::v4), 0));
    if (listener.has_error())
        return fixture;
    fixture.listener = cc::move(listener).value();
    return fixture;
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}
} // namespace

TEST("cnet - a connection is established and both ends know who they are")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    // Port 0 asked the OS to choose, which is what keeps two test servers from colliding.
    CHECK(server.where().port != 0);
    CHECK(server.where().address.is_loopback());

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());

    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& client = connected->value();
    auto const& peer = accepted->value();
    CHECK(client->is_open());
    CHECK(peer->is_open());

    // The accepted side sees the client's address, and both agree on the port the client came from.
    CHECK(peer->peer().address.is_loopback());
    CHECK(peer->local().port == server.where().port);
}

TEST("cnet - bytes go both ways")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());
    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    auto const greeting = cc::string_view("hello tcp");
    auto sent = client->send(bytes_of(greeting));

    byte inbox[64] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));

    CHECK(settled_ok(sent));
    CHECK(settled_ok(received));

    // A send completes only when every byte is gone; a receive completes on the first that arrive.
    CHECK(received->value() == greeting.size());
    CHECK(cc::string_view(reinterpret_cast<char const*>(inbox), received->value()) == greeting);

    // And back the other way, over the same pair.
    auto const answer = cc::string_view("hello yourself");
    auto replied = peer->send(bytes_of(answer));

    byte client_inbox[64] = {};
    auto client_received = client->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))));

    CHECK(settled_ok(replied));
    CHECK(settled_ok(client_received));
    CHECK(cc::string_view(reinterpret_cast<char const*>(client_inbox), client_received->value()) == answer);
}

TEST("cnet - a receive reports what arrived rather than filling its buffer")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());
    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    auto const four = cc::string_view("abcd");
    CHECK(settled_ok(client->send(bytes_of(four))));

    // A stream has no message boundaries, so a four-byte write answers a one-kilobyte read with four bytes.
    // Waiting to fill the buffer would be waiting for a message nobody sent.
    auto inbox = cc::vector<byte>();
    inbox.resize_to_defaulted(1024);
    auto received = peer->receive(inbox);
    CHECK(settled_ok(received));
    CHECK(received->value() == 4);
}

TEST("cnet - a peer that hangs up fails the read rather than reporting zero bytes")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());
    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& peer = accepted->value();
    connected->value()->close();

    byte inbox[16] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));

    CHECK(wait_for([&] { return received->is_ready(); }));
    CHECK(received->try_error() != nullptr);

    // A zero-byte success would be indistinguishable from a read that simply has not happened yet.
    CHECK(!received->try_error()->is_cancelled());
}

TEST("cnet - a refused connection fails, and says why")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    // Take the listener away, so its port has nothing behind it.
    auto const abandoned = server.where();
    server.listener = {};

    auto connected = tcp_connect(*server.io, abandoned, deadline::after_secs(5));
    CHECK(wait_for([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - an operation on a closed connection fails without reaching the reactor")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());
    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& client = connected->value();
    client->close();
    CHECK(!client->is_open());

    byte inbox[8] = {};
    auto received = client->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));

    // Already broken when handed back: no wait, no reactor round trip.
    CHECK(received->is_ready());
    CHECK(received->try_error() != nullptr);
}

TEST("cnet - a connect deadline is measured against the io_system's clock")
{
    auto clk = manual_clock(0);
    auto io = io_system::try_create({.unthreaded = true, .time_source = &clk});
    if (io.has_error())
        SKIP("this platform has no sockets");

    // 203.0.113.0/24 is TEST-NET-3, reserved by RFC 5737 for documentation.
    // Nothing routes there, so the connect hangs rather than being refused -- which is the case a deadline is for,
    // and the reason this needs no server at all.
    auto const nowhere = endpoint(ip_address::parse("203.0.113.1").value(), 9);
    auto connected = tcp_connect(*io.value(), nowhere, deadline::after_secs(30));

    CHECK(wait_for([&] { return io.value()->pending_count() == 1; }));
    CHECK(!connected->is_ready());

    // Thirty seconds pass in no time at all.
    clk.advance_ms(30'001);
    CHECK(wait_for([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - listening needs an address, and reports what it could not do")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    auto const nowhere = stream_listener::try_create(*io.value(), endpoint());
    CHECK(nowhere.has_error());
    CHECK(nowhere.error().code == error_code::invalid_argument);

    // Port 1 is privileged on every platform we target, so this is either a refusal or a machine running as root.
    auto const privileged = stream_listener::try_create(*io.value(), endpoint(ip_address::loopback(ip_family::v4), 1));
    if (privileged.has_error())
    {
        auto const expected = privileged.error().code == error_code::permission_denied
                           || privileged.error().code == error_code::address_in_use;
        CHECK(expected);
    }
}

TEST("cnet - a half-close ends the sending half and leaves the answer coming")
{
    auto server = make_server();
    if (!server.up())
        SKIP("this platform has no sockets");

    auto accepted = server.listener->accept();
    auto connected = tcp_connect(*server.io, server.where());
    CHECK(settled_ok(accepted));
    CHECK(settled_ok(connected));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    auto const request = cc::string_view("that is the whole request");
    CHECK(settled_ok(client->send(bytes_of(request))));
    CHECK(client->shutdown_send().has_value());

    // The request still arrives: a half-close ends the stream AFTER what was already written.
    byte inbox[64] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(settled_ok(received));
    CHECK(received->value() == request.size());

    // And the next read sees end-of-stream, which is how the peer knows the request is complete.
    auto ended = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(wait_for([&] { return ended->is_ready(); }));
    CHECK(ended->try_error() != nullptr);

    // The connection is still open the other way, which is the whole point of half-closing rather than closing.
    auto const answer = cc::string_view("and here is the answer");
    CHECK(settled_ok(peer->send(bytes_of(answer))));

    byte client_inbox[64] = {};
    auto client_received = client->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))));
    CHECK(settled_ok(client_received));
    CHECK(cc::string_view(reinterpret_cast<char const*>(client_inbox), client_received->value()) == answer);
}
