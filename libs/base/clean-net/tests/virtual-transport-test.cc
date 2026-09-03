#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The transport with no network under it: the same tcp_listener and tcp_connection a socket would have handed back,
// answered in this process.
// Every test runs unthreaded, and the ones that wait on something use a manual clock -- which is the point of the
// whole exercise, since nothing here can be slow or machine-dependent.

namespace
{
/// Drive the process pump until `done`, or give up after a bounded number of rounds.
///
/// A round count rather than a wall-clock budget: nothing here waits on the world, so a run that does not finish
/// promptly is a bug rather than a slow machine.
bool pump_until(cc::function_ref<bool()> done, i32 rounds = 1000)
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

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}

[[nodiscard]] endpoint somewhere(i32 port = 0)
{
    return endpoint(ip_address::loopback(ip_family::v4), port);
}
} // namespace

TEST("cnet - a virtual network refuses an endpoint nobody listens on")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    CHECK(net.is_supported());

    auto connected = tcp_connect(net, somewhere(1234));
    CHECK(connected->is_ready());
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - a virtual connection carries bytes both ways")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();

    // Port 0 is assigned here too, so the "bind to 0 and ask which port" pattern a test server uses still works.
    CHECK(listener->local().port != 0);

    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());

    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));
    CHECK(accepted->try_error() == nullptr);
    CHECK(connected->try_error() == nullptr);

    auto const& client = connected->value();
    auto const& peer = accepted->value();
    CHECK(client->peer() == listener->local());
    CHECK(peer->local() == listener->local());

    auto const greeting = cc::string_view("hello nowhere");
    auto sent = client->send(bytes_of(greeting));
    CHECK(sent->is_ready());
    CHECK(sent->try_error() == nullptr);

    byte inbox[64] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(received->is_ready()); // the bytes were already there, so nothing reached the reactor
    CHECK(received->value() == greeting.size());
    CHECK(cc::string_view(reinterpret_cast<char const*>(inbox), received->value()) == greeting);

    auto const answer = cc::string_view("hello yourself");
    CHECK(peer->send(bytes_of(answer))->try_error() == nullptr);

    byte client_inbox[64] = {};
    auto client_received = client->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))));
    CHECK(client_received->is_ready());
    CHECK(cc::string_view(reinterpret_cast<char const*>(client_inbox), client_received->value()) == answer);
}

TEST("cnet - a virtual read parks until the peer writes")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    byte inbox[16] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));

    // Nothing to read, so this one is a pending reactor operation rather than an answered promise.
    CHECK(!received->is_ready());
    CHECK(pump_until([&] { return io->pending_count() == 1; }));

    auto const late = cc::string_view("late");
    CHECK(client->send(bytes_of(late))->try_error() == nullptr);

    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() == nullptr);
    CHECK(received->value() == late.size());
}

TEST("cnet - a virtual accept parks until somebody connects")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    CHECK(!accepted->is_ready());

    auto connected = tcp_connect(net, listener->local());
    CHECK(connected->is_ready()); // a virtual connect never waits: the listener is either there or it is not

    CHECK(pump_until([&] { return accepted->is_ready(); }));
    CHECK(accepted->try_error() == nullptr);
}

TEST("cnet - a virtual read honours its deadline against the injected clock")
{
    auto clk = manual_clock(0);
    auto io = io_system::create({.unthreaded = true, .time_source = &clk});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    byte inbox[16] = {};
    auto received = accepted->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))), deadline::after_secs(30));
    CHECK(!received->is_ready());

    // The deadline is the io_system's, so a virtual transport gets the real one rather than a second mechanism.
    clk.advance_ms(30'001);
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() != nullptr);
}

TEST("cnet - closing a virtual connection ends the peer's read")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const& peer = accepted->value();

    byte inbox[16] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(!received->is_ready());

    connected->value()->close();

    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() != nullptr);

    // A read after the peer is gone fails without reaching the reactor at all.
    auto again = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(again->is_ready());
    CHECK(again->try_error() != nullptr);
}

TEST("cnet - a virtual half-close leaves the answer coming")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = tcp_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    auto const request = cc::string_view("the whole request");
    CHECK(client->send(bytes_of(request))->try_error() == nullptr);
    CHECK(client->shutdown_send().has_value());

    byte inbox[64] = {};
    auto received = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(received->is_ready());
    CHECK(received->value() == request.size());

    // What was written still arrives, and only then does the stream end.
    auto ended = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(ended->is_ready());
    CHECK(ended->try_error() != nullptr);

    // The other direction is untouched, which is the whole point of half-closing.
    auto const answer = cc::string_view("the answer");
    CHECK(peer->send(bytes_of(answer))->try_error() == nullptr);

    byte client_inbox[64] = {};
    auto client_received = client->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))));
    CHECK(client_received->is_ready());
    CHECK(cc::string_view(reinterpret_cast<char const*>(client_inbox), client_received->value()) == answer);
}

TEST("cnet - two virtual listeners cannot hold the same endpoint")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto const first = tcp_listener::try_create(net, somewhere(8080));
    CHECK(first.has_value());

    auto const second = tcp_listener::try_create(net, somewhere(8080));
    CHECK(second.has_error());
    CHECK(second.error().code == error_code::address_in_use);
}
