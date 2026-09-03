#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/connect.hh>
#include <clean-net/transport/simulated_transport.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Connecting to a name, and the race that makes a broken IPv6 route cost milliseconds instead of a timeout.
// Everything runs over a virtual network with an injected lookup and a manual clock, so "the v6 address is a black
// hole" is a fact of the fixture rather than a property of the machine.

namespace
{
bool pump_until(cc::function_ref<bool()> done, i32 rounds = 4000)
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

[[nodiscard]] ip_address addr(cc::string_view text)
{
    return ip_address::parse(text).value();
}

/// A virtual network, a resolver answering from a table, and the clock both are measured against.
struct host_fixture
{
    manual_clock clk = manual_clock(0);
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<resolver> res;

    explicit host_fixture(cc::vector<ip_address> answers)
    {
        io = io_system::create({.unthreaded = true, .time_source = &clk});
        net = cc::make_unique<virtual_network>(*io);
        res = resolver::create(
            *io, {.lookup = [answers = cc::move(answers)](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                  { return answers; }});
    }

    /// Listen on one of the answers, so exactly that address is the one that can be connected to.
    [[nodiscard]] cc::unique_ptr<stream_listener> listen_on(ip_address const& a, i32 port)
    {
        return stream_listener::try_create(*net, endpoint(a, port)).value();
    }
};
} // namespace

TEST("cnet - connecting to a name resolves and connects")
{
    auto fixture = host_fixture({addr("10.0.0.1")});
    auto listener = fixture.listen_on(addr("10.0.0.1"), 8080);
    auto accepted = listener->accept();

    auto connected = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 8080);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(connected->value()->peer() == endpoint(addr("10.0.0.1"), 8080));

    CHECK(pump_until([&] { return accepted->is_ready(); }));
}

TEST("cnet - a port that is not a port is refused before anything happens")
{
    auto fixture = host_fixture({addr("10.0.0.1")});

    auto const bad = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 0);
    CHECK(bad->is_ready());
    CHECK(bad->try_error() != nullptr);

    auto const worse = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 70000);
    CHECK(worse->is_ready());
    CHECK(worse->try_error() != nullptr);
}

TEST("cnet - the race reaches the family that works when the other one does not")
{
    // The v6 address is a black hole: nothing listens there, so an attempt to it is refused.
    // The v4 address is the one with a server behind it.
    auto fixture = host_fixture({addr("2001:db8::1"), addr("10.0.0.1")});
    auto listener = fixture.listen_on(addr("10.0.0.1"), 443);
    auto accepted = listener->accept();

    auto connected = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 443);

    // IPv6 leads and fails at once, so the v4 attempt starts without waiting for the stagger.
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(connected->value()->peer().address.family() == ip_family::v4);

    CHECK(pump_until([&] { return accepted->is_ready(); }));
}

TEST("cnet - every address failing reports the first attempt's failure")
{
    auto fixture = host_fixture({addr("2001:db8::1"), addr("10.0.0.1")});

    // Nothing listens anywhere, so both attempts are refused.
    auto connected = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 443);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() != nullptr);
    CHECK(!connected->try_error()->is_cancelled());
}

TEST("cnet - a name that does not resolve fails the connect")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);
    auto res = resolver::create(*io, {.lookup = [](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                      {
                                          return cc::error(error{.code = error_code::name_not_resolved,
                                                                 .native_code = 0,
                                                                 .message = cc::string("no such host")});
                                      }});

    auto connected = connect_to_host(net, *res, "nothing.invalid", 80);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - cancelling the caller's token cancels the whole race")
{
    auto fixture = host_fixture({addr("2001:db8::1")});

    // Nobody is listening, and nothing will start listening, so the race is still going when we change our mind.
    auto const token = cancel_token::create();
    auto connected
        = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 443, {.timeout = deadline::never()}, token);

    // The v6 attempt is refused at once by the virtual network, so what is left is the race itself.
    token.cancel();
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - a race that wins leaves the caller's token alone")
{
    auto fixture = host_fixture({addr("10.0.0.1")});
    auto listener = fixture.listen_on(addr("10.0.0.1"), 8080);
    auto accepted = listener->accept();

    auto const token = cancel_token::create();
    auto connected = connect_to_host(*fixture.net, *fixture.res, "example.invalid", 8080, {}, token);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(pump_until([&] { return accepted->is_ready(); }));

    // Cancelling the losers must not cancel the caller: that is what a CHILD token is for.
    CHECK(!token.is_cancelled());

    // And the connection the race handed back is a working one.
    auto const message = cc::string_view("through the race");
    auto sent
        = connected->value()->send(cc::span<byte const>(reinterpret_cast<byte const*>(message.data()), message.size()));
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);
}

TEST("cnet - a child token cancels with its parent, and alone")
{
    auto const parent = cancel_token::create();
    auto const child = parent.create_child();

    CHECK(child.is_valid());
    CHECK(!child.is_cancelled());

    // Downward only: the child is what an operation gets, so finishing it must not reach the request above.
    child.cancel();
    CHECK(child.is_cancelled());
    CHECK(!parent.is_cancelled());

    auto const other = parent.create_child();
    parent.cancel();
    CHECK(other.is_cancelled());

    // A child born after the cancel is born cancelled, so a late caller fails at once.
    auto const late = parent.create_child();
    CHECK(late.is_cancelled());

    // A child of the default token is simply an independent token: there is nothing above it to answer to.
    auto const orphan = cancel_token().create_child();
    CHECK(orphan.is_valid());
    CHECK(!orphan.is_cancelled());
}

TEST("cnet - the stagger starts another attempt, and the losers stop when the race is decided")
{
    // Three addresses, all reachable, on a link slow enough that the first attempt is still in flight when the
    // stagger fires -- which is the only way to watch the stagger do its job.
    auto fixture = host_fixture({addr("2001:db8::1"), addr("10.0.0.1"), addr("2001:db8::2")});
    auto link = simulated_transport(*fixture.io, *fixture.net, {.latency_ms = 10'000});

    auto listener_first = stream_listener::try_create(*fixture.net, endpoint(addr("2001:db8::1"), 443)).value();
    auto listener_second = stream_listener::try_create(*fixture.net, endpoint(addr("10.0.0.1"), 443)).value();
    auto accepted_first = listener_first->accept();
    auto accepted_second = listener_second->accept();

    // Resolve up front, so the race starts at once and the clock below measures only the connecting.
    auto warm = fixture.res->resolve("example.invalid");
    CHECK(pump_until([&] { return warm->is_ready(); }));

    auto connected = connect_to_host(link, *fixture.res, "example.invalid", 443,
                                     {.timeout = deadline::never(), .attempt_delay_ms = 250});

    // One attempt in flight and one stagger waiting to start the next, beside the two parked accepts.
    // Nothing has reached a listener yet: the link's latency is paid before the connect underneath it happens.
    CHECK(pump_until([&] { return fixture.io->pending_count() > 0; }));
    CHECK(!accepted_first->is_ready());
    auto const before_stagger = fixture.io->pending_count();

    // The stagger fires and starts the second attempt BESIDE the first rather than after it, which is the one thing
    // that separates a race from a fallback.
    fixture.clk.advance_ms(250);
    CHECK(pump_until([&] { return fixture.io->pending_count() > before_stagger; }));

    // The first attempt lands first, because it started 250 ms earlier on an equally slow link.
    fixture.clk.advance_ms(9'800);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(connected->value()->peer() == endpoint(addr("2001:db8::1"), 443));
    CHECK(pump_until([&] { return accepted_first->is_ready(); }));

    // The losers were cancelled the moment the race was decided, so they never reach their servers at all --
    // the second listener is still waiting for a connection that will now never arrive.
    fixture.clk.advance_ms(20'000);
    CHECK(pump_until([&] { return true; }, 50));
    CHECK(!accepted_second->is_ready());

    // And the connection that won is a working one.
    auto const message = cc::string_view("the winner speaks");
    auto sent
        = connected->value()->send(cc::span<byte const>(reinterpret_cast<byte const*>(message.data()), message.size()));
    fixture.clk.advance_ms(10'000);
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);
}

TEST("cnet - tearing down an io_system abandons what is still in flight")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = cc::make_unique<virtual_network>(*io);
    auto res = resolver::create(*io, {.lookup = [](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                      { return cc::vector<ip_address>{addr("127.0.0.1")}; }});

    // Parked rather than finished: nothing pumps, so the resolve is still sitting in the reactor.
    auto connecting = connect_to_host(*net, *res, "example.test", 80);
    CHECK(!connecting->is_ready());

    // THE IO_SYSTEM GOES FIRST, which is the order that makes this safe and the reason it is asserted here.
    // Until it is gone, any thread in the process can drive it through `cc::thread_pump_all()`, and a completion
    // reaches back into whatever started the operation -- so a transport destroyed before it is a transport a
    // completion can still find.
    // Its own teardown then abandons what is pending rather than completing it, which is what `~reactor` has always
    // said and what the actor's drain used to violate.
    io = {};
    net = {};
    res = {};

    // Never settled, which is the abandonment being asserted rather than a leak.
    CHECK(!connecting->is_ready());
}
