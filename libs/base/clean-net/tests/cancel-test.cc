#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/transport/simulated_transport.hh>
#include <clean-net/transport/stream.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Cancelling operations that are already under way, over every transport there is.
// A token groups where a deadline bounds, so the interesting cases are one token cancelling several operations at
// once, and a cancel that arrives before anything started.

namespace
{
bool pump_until(cc::function_ref<bool()> done, i32 rounds = 2000)
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

/// Pump against the wall clock rather than a round count.
///
/// A round budget measures how fast this machine spins, and nexus runs these tests alongside each other -- so a real
/// socket, which waits on the world, needs a clock and not a counter.
bool pump_for(cc::function_ref<bool()> done, f64 budget_secs = 10.0)
{
    auto& clk = system_clock();
    auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

    while (true)
    {
        if (done())
            return true;
        if (clk.now_ns() >= deadline_ns)
            return false;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
}

[[nodiscard]] endpoint somewhere()
{
    return endpoint(ip_address::loopback(ip_family::v4), 0);
}

/// Whether a settled async failed the way a cancel is supposed to look.
template <class T>
[[nodiscard]] bool reads_as_cancelled(cc::shared_async<T> const& handle)
{
    auto const* const failure = handle->try_error();
    return failure != nullptr && failure->is_cancelled();
}
} // namespace

TEST("cnet - the default token cancels nothing and allocates nothing")
{
    auto const none = cancel_token();
    CHECK(!none.is_valid());
    CHECK(!none.is_cancelled());

    // Cancelling it is legal and does nothing, so a caller never has to branch on having one.
    none.cancel();
    CHECK(!none.is_cancelled());

    auto const real = cancel_token::create();
    CHECK(real.is_valid());
    CHECK(!real.is_cancelled());
    real.cancel();
    CHECK(real.is_cancelled());

    // A copy shares the same control block, which is what lets one token be handed to four operations.
    auto copy = real;
    CHECK(copy.is_cancelled());

    // And dropping a copy leaves the original -- and its block -- alone.
    copy = cancel_token();
    CHECK(!copy.is_valid());
    CHECK(real.is_cancelled());
}

TEST("cnet - a cancelled token fails an operation before it starts")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto const token = cancel_token::create();
    token.cancel();

    auto listener = stream_listener::try_create(net, somewhere()).value();
    auto connected = tcp_connect(net, listener->local(), deadline::after_secs(5), {}, token);

    // No reactor round trip: a token that is already cancelled means the work was never wanted.
    CHECK(connected->is_ready());
    CHECK(reads_as_cancelled(connected));
    CHECK(io->pending_count() == 0);
}

TEST("cnet - cancelling ends a parked virtual read")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = stream_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const token = cancel_token::create();

    byte inbox[16] = {};
    auto received = accepted->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))), deadline::never(), token);
    CHECK(!received->is_ready());

    // No deadline at all, so cancellation is the only thing that can end this.
    token.cancel();
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(reads_as_cancelled(received));
    CHECK(pump_until([&] { return io->pending_count() == 0; }));
}

TEST("cnet - one token cancels every operation it was given to")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = stream_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const token = cancel_token::create();

    // A read on each end of one connection, and an accept nobody will ever connect to: the shape of a request that
    // has several things in flight when the caller changes its mind.
    byte server_inbox[16] = {};
    byte client_inbox[16] = {};
    auto server_read = accepted->value()->receive(cc::span<byte>(server_inbox, isize(sizeof(server_inbox))),
                                                  deadline::never(), token);
    auto client_read = connected->value()->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))),
                                                   deadline::never(), token);
    auto second_accept = listener->accept(deadline::never(), token);

    CHECK(pump_until([&] { return io->pending_count() == 3; }));

    token.cancel();

    CHECK(pump_until([&] { return server_read->is_ready() && client_read->is_ready() && second_accept->is_ready(); }));
    CHECK(reads_as_cancelled(server_read));
    CHECK(reads_as_cancelled(client_read));
    CHECK(reads_as_cancelled(second_accept));
}

TEST("cnet - cancelling after an operation finished is harmless")
{
    auto io = io_system::create({.unthreaded = true});
    auto net = virtual_network(*io);

    auto listener = stream_listener::try_create(net, somewhere()).value();
    auto accepted = listener->accept(deadline::never(), cancel_token());
    auto connected = tcp_connect(net, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const token = cancel_token::create();

    auto const message = cc::string_view("done already");
    auto sent
        = connected->value()->send(cc::span<byte const>(reinterpret_cast<byte const*>(message.data()), message.size()),
                                   deadline::after_secs(5), token);
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);

    // The operation deregistered itself when it completed, so this reaches nothing at all.
    token.cancel();
    CHECK(sent->try_error() == nullptr);
}

TEST("cnet - cancelling a simulated link does not wait its latency out")
{
    auto clk = manual_clock(0);
    auto io = io_system::create({.unthreaded = true, .time_source = &clk});
    auto net = virtual_network(*io);
    auto link = simulated_transport(*io, net, {.latency_ms = 5000});

    auto listener = stream_listener::try_create(link, somewhere()).value();

    auto const token = cancel_token::create();
    auto connected = tcp_connect(link, listener->local(), deadline::never(), {}, token);

    // Parked on the link's own delay timer, with a clock nobody is going to advance.
    CHECK(!connected->is_ready());
    CHECK(pump_until([&] { return io->pending_count() >= 1; }));

    token.cancel();
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(reads_as_cancelled(connected));
}

TEST("cnet - cancelling ends a parked read on a real socket")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    auto listener = stream_listener::try_create(*io.value(), somewhere());
    if (listener.has_error())
        SKIP("this platform has no sockets");

    auto accepted = listener.value()->accept();
    auto connected = tcp_connect(*io.value(), listener.value()->local());
    CHECK(pump_for([&] { return accepted->is_ready() && connected->is_ready(); }));
    CHECK(accepted->try_error() == nullptr);

    auto const token = cancel_token::create();

    byte inbox[16] = {};
    auto received = accepted->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))), deadline::never(), token);
    CHECK(!received->is_ready());

    token.cancel();
    CHECK(pump_for([&] { return received->is_ready(); }));
    CHECK(reads_as_cancelled(received));

    // The socket outlives the operation, so the connection is still usable afterwards -- a cancel ends an operation
    // rather than the connection it ran on.
    CHECK(accepted->value()->is_open());
}
