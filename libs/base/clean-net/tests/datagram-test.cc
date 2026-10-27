#include "cnet-test-types.hh"

#include <clean-core/common/time.hh> // cc::current_time_steady_secs
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/datagram.hh>
#include <clean-net/transport/simulated_transport.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

#include <memory> // std::make_unique, to hand datagram_socket a test backend through its interface

using namespace cc::primitive_defines;

using namespace cnet;

// Datagrams over all three transports.
// The native ones run over loopback and wait for real arrivals; the virtual and simulated ones run on a manual clock,
// so a 50 ms link costs nothing and every arrival time is exact.

namespace
{
/// Bounded by the wall clock, as in the other transport tests: the bound is a timeout, never the thing measured.
bool pump_until(cc::function_ref<bool()> done, double max_ms = 5000)
{
    auto const started = cc::current_time_steady_secs();
    while (true)
    {
        if (done())
            return true;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
        if ((cc::current_time_steady_secs() - started) * 1000.0 >= max_ms)
            return done();
    }
}

[[nodiscard]] endpoint loopback_any_port()
{
    return endpoint(ip_address::loopback(ip_family::v4), 0);
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}

[[nodiscard]] cc::string_view text_of(datagram const& d)
{
    return cc::string_view(reinterpret_cast<char const*>(d.bytes().data()), d.size);
}

/// Receive records over one buffer, set up once and reused unchanged, the way a caller is meant to.
struct inbox
{
    cc::vector<byte> storage;
    cc::vector<datagram> records;

    explicit inbox(isize count, isize slot = 2048)
    {
        storage.resize_to_defaulted(count * slot);
        records.resize_to_defaulted(count);
        for (isize i = 0; i < count; ++i)
            records[i].buffer = cc::span<byte>(storage).subspan(i * slot).first_n(slot);
    }

    [[nodiscard]] cc::span<datagram> all() { return records; }
};

/// A virtual network, optionally with a simulated link over it, on a clock the test moves.
struct world
{
    manual_clock clk = manual_clock(1'000'000'000);
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<simulated_transport> link;

    explicit world(cc::optional<link_conditions> conditions = {})
    {
        io = io_system::create({.unthreaded = true, .time_source = &clk});
        net = cc::make_unique<virtual_network>(*io);
        if (conditions.has_value())
            link = cc::make_unique<simulated_transport>(*io, *net, conditions.value());
    }

    [[nodiscard]] transport& t() { return link.is_valid() ? static_cast<transport&>(*link) : *net; }
};

/// Send `count` datagrams carrying their index as one byte.
void send_numbered(datagram_socket& from, endpoint to, isize count)
{
    auto payload = cc::vector<byte>();
    auto packets = cc::vector<outgoing_datagram>();
    payload.resize_to_defaulted(count);
    for (isize i = 0; i < count; ++i)
        payload[i] = byte(i);
    for (isize i = 0; i < count; ++i)
        packets.push_back({.peer = to, .bytes = cc::span<byte const>(payload).subspan(i).first_n(1)});
    CHECK(from.send_batch(packets).value() == count);
}
} // namespace

// ---- native ----------------------------------------------------------------------------------------------

CNET_IO_TEST("cnet - datagrams over loopback arrive whole, with their sender and an arrival time")
{
    auto io = io_system::create({.unthreaded = true});
    auto a = datagram_socket::try_create(*io, loopback_any_port());
    if (!native_transport(*io).is_supported())
    {
        CHECK(a.error().code == error_code::unsupported);
        return;
    }
    auto b = datagram_socket::create(*io, loopback_any_port());

    auto const words = cc::vector<cc::string_view>{"first", "second", "third"};
    auto packets = cc::vector<outgoing_datagram>();
    for (auto w : words)
        packets.push_back({.peer = b->local(), .bytes = bytes_of(w)});
    CHECK(a.value()->send_batch(packets).value() == 3);

    auto in = inbox(8);
    auto got = cc::vector<cc::string>();
    CHECK(pump_until(
        [&]
        {
            auto const n = b->receive_batch(in.all()).value();
            for (isize i = 0; i < n; ++i)
            {
                CHECK(in.records[i].peer == a.value()->local());
                CHECK(in.records[i].received_at_ns > 0);
                CHECK(!in.records[i].truncated);
                got.push_back(cc::string(text_of(in.records[i])));
            }
            return got.size() == 3;
        }));

    // Loopback keeps order in practice, and nothing here depends on it beyond this check.
    CHECK(got[0] == "first");
    CHECK(got[1] == "second");
    CHECK(got[2] == "third");

    // The records are reusable as they are: a receive never shrinks the room it was given.
    CHECK(in.records[0].buffer.size() == 2048);
}

CNET_IO_TEST("cnet - an empty native socket answers zero rather than waiting or failing")
{
    auto io = io_system::create({.unthreaded = true});
    if (!native_transport(*io).is_supported())
        return;

    auto s = datagram_socket::create(*io, loopback_any_port());
    auto in = inbox(4);
    CHECK(s->receive_batch(in.all()).value() == 0);
}

CNET_IO_TEST("cnet - a native datagram larger than its room is cut, and says so")
{
    auto io = io_system::create({.unthreaded = true});
    if (!native_transport(*io).is_supported())
        return;

    auto a = datagram_socket::create(*io, loopback_any_port());
    auto b = datagram_socket::create(*io, loopback_any_port());

    auto const long_one = cc::string_view("considerably longer than four bytes");
    auto const packet = outgoing_datagram{.peer = b->local(), .bytes = bytes_of(long_one)};
    CHECK(a->send_batch(cc::span<outgoing_datagram const>(&packet, 1)).value() == 1);

    auto in = inbox(1, 4);
    auto n = isize(0);
    CHECK(pump_until([&] { return (n = b->receive_batch(in.all()).value()) > 0; }));
    CHECK(n == 1);
    CHECK(in.records[0].truncated);
    CHECK(in.records[0].size == 4);
    CHECK(text_of(in.records[0]) == "cons");
}

CNET_IO_TEST("cnet - a peer that went away is silence on the native socket, not an error")
{
    auto io = io_system::create({.unthreaded = true});
    if (!native_transport(*io).is_supported())
        return;

    auto a = datagram_socket::create(*io, loopback_any_port());
    auto gone = datagram_socket::create(*io, loopback_any_port());
    auto const dead_end = gone->local();
    gone = {};

    // What this pins is the property, not the mechanism: whenever the ICMP "port unreachable" for this send arrives,
    // no receive on `a` fails because of it.
    // Windows reports one as WSAECONNRESET on the next receive unless the socket turned that off, and the receive path
    // skips it anyway; which of the two kept this green depends on when the reply lands.
    auto const into_the_void = outgoing_datagram{.peer = dead_end, .bytes = bytes_of("anyone?")};
    CHECK(a->send_batch(cc::span<outgoing_datagram const>(&into_the_void, 1)).value() == 1);

    auto c = datagram_socket::create(*io, loopback_any_port());
    auto const hello = outgoing_datagram{.peer = a->local(), .bytes = bytes_of("hello")};
    CHECK(c->send_batch(cc::span<outgoing_datagram const>(&hello, 1)).value() == 1);

    auto in = inbox(4);
    auto received = false;
    CHECK(pump_until(
        [&]
        {
            auto got = a->receive_batch(in.all());
            CHECK(got.has_value());
            if (got.has_value() && got.value() > 0)
                received = text_of(in.records[0]) == "hello";
            return received;
        }));
}

CNET_IO_TEST("cnet - a native send batch reports how far it got, and the next call says why it stopped")
{
    auto io = io_system::create({.unthreaded = true});
    if (!native_transport(*io).is_supported())
        return;

    auto a = datagram_socket::create(*io, loopback_any_port());
    auto b = datagram_socket::create(*io, loopback_any_port());

    auto const packets = cc::vector<outgoing_datagram>{
        {.peer = b->local(), .bytes = bytes_of("fine")},
        {.peer = endpoint(), .bytes = bytes_of("nowhere")},
        {.peer = b->local(), .bytes = bytes_of("never reached")},
    };

    CHECK(a->send_batch(packets).value() == 1);

    auto const rest = a->send_batch(cc::span<outgoing_datagram const>(packets).subspan(1));
    REQUIRE(rest.has_error());
    CHECK(rest.error().code == error_code::invalid_argument);
}

CNET_IO_TEST("cnet - the native arrival callback is level-triggered")
{
    auto io = io_system::create({.unthreaded = true});
    if (!native_transport(*io).is_supported())
        return;

    auto a = datagram_socket::create(*io, loopback_any_port());
    auto b = datagram_socket::create(*io, loopback_any_port());

    auto in = inbox(4);
    auto calls = 0;
    auto drained = isize(0);

    // The first call leaves the datagram where it is, so the socket stays readable and the callback runs again.
    b->on_arrival(
        [&](datagram_socket& s)
        {
            ++calls;
            if (calls >= 2)
                drained += s.receive_batch(in.all()).value();
        });

    auto const hello = outgoing_datagram{.peer = b->local(), .bytes = bytes_of("hello")};
    CHECK(a->send_batch(cc::span<outgoing_datagram const>(&hello, 1)).value() == 1);

    CHECK(pump_until([&] { return drained == 1; }));
    CHECK(calls >= 2);

    b->on_arrival({});
}

// ---- virtual -------------------------------------------------------------------------------------------

CNET_IO_TEST("cnet - a virtual datagram arrives at once, stamped with the moment it was sent")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    // A port of 0 is assigned one, so the "bind and ask" pattern works here too.
    CHECK(a->local().port != 0);
    CHECK(a->local().port != b->local().port);

    auto const hello = outgoing_datagram{.peer = b->local(), .bytes = bytes_of("hello")};
    CHECK(a->send_batch(cc::span<outgoing_datagram const>(&hello, 1)).value() == 1);

    auto in = inbox(4);
    CHECK(b->receive_batch(in.all()).value() == 1);
    CHECK(text_of(in.records[0]) == "hello");
    CHECK(in.records[0].peer == a->local());
    CHECK(in.records[0].received_at_ns == 1'000'000'000);
    CHECK(b->receive_batch(in.all()).value() == 0);
}

CNET_IO_TEST("cnet - a virtual datagram to nobody vanishes, and a taken endpoint refuses a second bind")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto const lost = outgoing_datagram{.peer = endpoint(ip_address::loopback(ip_family::v4), 9), .bytes = bytes_of("?")};
    CHECK(a->send_batch(cc::span<outgoing_datagram const>(&lost, 1)).value() == 1);

    auto again = datagram_socket::try_create(*w.net, a->local());
    REQUIRE(again.has_error());
    CHECK(again.error().code == error_code::address_in_use);
}

CNET_IO_TEST("cnet - a virtual socket bound to every interface receives what is sent to one of them")
{
    auto w = world();
    auto server = datagram_socket::try_create(*w.net, endpoint(ip_address::any(ip_family::v4), 7777)).value();
    auto client = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto const hello
        = outgoing_datagram{.peer = endpoint(ip_address::loopback(ip_family::v4), 7777), .bytes = bytes_of("hi")};
    CHECK(client->send_batch(cc::span<outgoing_datagram const>(&hello, 1)).value() == 1);

    auto in = inbox(1);
    CHECK(server->receive_batch(in.all()).value() == 1);
    CHECK(in.records[0].peer == client->local());
}

CNET_IO_TEST("cnet - a virtual datagram larger than any network carries is refused under the partial-batch rule")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto huge = cc::vector<byte>();
    huge.resize_to_defaulted(70'000);
    auto const packets = cc::vector<outgoing_datagram>{
        {.peer = b->local(), .bytes = bytes_of("small")},
        {.peer = b->local(), .bytes = huge},
    };

    CHECK(a->send_batch(packets).value() == 1);

    auto const rest = a->send_batch(cc::span<outgoing_datagram const>(packets).subspan(1));
    REQUIRE(rest.has_error());
    CHECK(rest.error().code == error_code::invalid_argument);
}

CNET_IO_TEST("cnet - the virtual arrival callback runs once per wake and stops when cleared")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto in = inbox(16);
    auto calls = 0;
    auto received = isize(0);
    b->on_arrival(
        [&](datagram_socket& s)
        {
            ++calls;
            received += s.receive_batch(in.all()).value();
        });

    // Five datagrams before anyone pumps are one wake and one drain, not five calls.
    send_numbered(*a, b->local(), 5);
    CHECK(pump_until([&] { return received == 5; }));
    CHECK(calls == 1);

    b->on_arrival({});
    send_numbered(*a, b->local(), 1);
    for (auto i = 0; i < 10; ++i)
        (void)cc::thread_pump_all();
    CHECK(calls == 1);
}

CNET_IO_TEST("cnet - a callback that replaces itself leaves one wait, not two")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto in = inbox(16);
    auto second_calls = 0;
    b->on_arrival(
        [&](datagram_socket& s)
        {
            (void)s.receive_batch(in.all());
            s.on_arrival(
                [&](datagram_socket& t)
                {
                    ++second_calls;
                    (void)t.receive_batch(in.all());
                });
        });

    send_numbered(*a, b->local(), 1);
    CHECK(pump_until([&] { return w.io->pending_count() == 1; }));

    // Two waits would each fire on the next datagram, and the second one would stay parked forever.
    send_numbered(*a, b->local(), 1);
    CHECK(pump_until([&] { return second_calls == 1; }));
    for (auto i = 0; i < 10; ++i)
        (void)cc::thread_pump_all();
    CHECK(second_calls == 1);
    CHECK(w.io->pending_count() == 1);

    b->on_arrival({});
}

CNET_IO_TEST("cnet - a callback may destroy its own socket")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto const where = b->local();

    auto in = inbox(4);
    auto calls = 0;
    b->on_arrival(
        [&](datagram_socket& s)
        {
            ++calls;
            (void)s.receive_batch(in.all());
            b = {};
        });

    send_numbered(*a, where, 1);
    CHECK(pump_until([&] { return !b.is_valid(); }));

    send_numbered(*a, where, 1);
    CHECK(pump_until([&] { return w.io->pending_count() == 0; }));
    CHECK(calls == 1);
}

CNET_IO_TEST("cnet - with a reactor thread, the arrival callback runs there and a destroy waits it out")
{
    auto io = io_system::create();
    auto net = virtual_network(*io);
    auto a = datagram_socket::try_create(net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(net, loopback_any_port()).value();

    // Sent before the callback is set, so the first wait can finish on the reactor thread before its hook is in.
    send_numbered(*a, b->local(), 1);

    auto const test_thread = cc::native_thread_id();
    auto ran_elsewhere = cc::atomic<bool>(true);
    auto inside = cc::atomic<bool>(false);
    auto finished = cc::atomic<bool>(false);
    auto in = inbox(16);
    b->on_arrival(
        [&](datagram_socket& s)
        {
            if (cc::native_thread_id() == test_thread)
                ran_elsewhere.store(false);
            (void)s.receive_batch(in.all());

            // Held open long enough that the destroy below lands while this call is still running.
            inside.store(true);
            cc::this_thread_sleep_secs(0.02);
            finished.store(true);
        });

    CHECK(pump_until([&] { return inside.load(); }));
    b = {};
    CHECK(finished.load());
    CHECK(ran_elsewhere.load());

    a = {};
    io->stop();
}

CNET_IO_TEST("cnet - stopping the io_system with a datagram already waiting does not call the callback")
{
    auto w = world();
    auto a = datagram_socket::try_create(*w.net, loopback_any_port()).value();
    auto b = datagram_socket::try_create(*w.net, loopback_any_port()).value();

    auto calls = 0;
    b->on_arrival([&](datagram_socket&) { ++calls; });

    // Delivered, and its wait signalled, before the stop; nothing has pumped yet.
    send_numbered(*a, b->local(), 1);
    w.io->stop();
    for (auto i = 0; i < 10; ++i)
        (void)cc::thread_pump_all();
    CHECK(calls == 0);
}

namespace
{
/// A backend whose first wait has already finished when it is handed back, as one the reactor thread completed before
/// the hook went in looks to the code installing it; every later wait never finishes.
class already_ready_backend final : public datagram_backend
{
public:
    explicit already_ready_backend(io_system& io) : _io(io) {}

    [[nodiscard]] cc::result<isize, error> receive_batch(cc::span<datagram>) override { return isize(0); }
    [[nodiscard]] cc::result<isize, error> send_batch(cc::span<outgoing_datagram const>) override { return isize(0); }

    [[nodiscard]] cc::shared_async<cc::unit> wait_readable(cancel_token const&) override
    {
        auto wait = cc::make_async_manual<cc::unit>();
        if (_first)
            wait->push_value(cc::unit{});
        _first = false;
        return wait;
    }

    [[nodiscard]] endpoint local() const override { return {}; }
    [[nodiscard]] io_system& io() const override { return _io; }

private:
    io_system& _io;
    bool _first = true;
};
} // namespace

CNET_IO_TEST("cnet - a wait that finished before its hook went in is called from the reactor, never inline")
{
    auto io = io_system::create({.unthreaded = true});

    {
        auto s = datagram_socket(std::make_unique<already_ready_backend>(*io));
        auto calls = 0;
        s.on_arrival([&](datagram_socket&) { ++calls; });

        // Not on this thread, inside on_arrival: a caller holding a lock its callback takes would deadlock there.
        CHECK(calls == 0);
        CHECK(pump_until([&] { return calls == 1; }));
        s.on_arrival({});
    }

    {
        // And never across a stop: the repost belongs to the reactor, and a stop settles it uncalled.
        auto s = datagram_socket(std::make_unique<already_ready_backend>(*io));
        auto calls = 0;
        s.on_arrival([&](datagram_socket&) { ++calls; });
        io->stop();
        for (auto i = 0; i < 10; ++i)
            (void)cc::thread_pump_all();
        CHECK(calls == 0);
    }
}

CNET_IO_TEST("cnet - a virtual bind clashes with one on every interface, as a real stack's does")
{
    auto w = world();
    auto const any_7000 = endpoint(ip_address::any(ip_family::v4), 7000);
    auto const loop_7000 = endpoint(ip_address::loopback(ip_family::v4), 7000);

    {
        auto wide = datagram_socket::try_create(*w.net, any_7000).value();
        auto narrow = datagram_socket::try_create(*w.net, loop_7000);
        REQUIRE(narrow.has_error());
        CHECK(narrow.error().code == error_code::address_in_use);
    }
    {
        auto narrow = datagram_socket::try_create(*w.net, loop_7000).value();
        auto wide = datagram_socket::try_create(*w.net, any_7000);
        REQUIRE(wide.has_error());
        CHECK(wide.error().code == error_code::address_in_use);
    }

    // Another family on the same port is a different socket altogether.
    auto v4 = datagram_socket::try_create(*w.net, any_7000).value();
    CHECK(datagram_socket::try_create(*w.net, endpoint(ip_address::any(ip_family::v6), 7000)).has_value());
}

CNET_IO_TEST("cnet - a virtual bind to port 0 skips a port bound explicitly")
{
    auto w = world();

    // 40000 is where the virtual network starts handing out ports.
    auto taken = datagram_socket::try_create(*w.net, endpoint(ip_address::loopback(ip_family::v4), 40000)).value();
    auto picked = datagram_socket::try_create(*w.net, loopback_any_port());
    REQUIRE(picked.has_value());
    CHECK(picked.value()->local().port != 40000);
}

// ---- simulated -----------------------------------------------------------------------------------------

CNET_IO_TEST("cnet - a simulated datagram link with no conditions is the network underneath")
{
    auto w = world(link_conditions{});
    auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
    auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

    send_numbered(*a, b->local(), 3);

    auto in = inbox(8);
    CHECK(b->receive_batch(in.all()).value() == 3);
    CHECK(in.records[0].received_at_ns == 1'000'000'000);
}

CNET_IO_TEST("cnet - simulated latency holds a datagram until the clock has moved past it")
{
    auto w = world(link_conditions{.latency_ms = 50});
    auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
    auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

    send_numbered(*a, b->local(), 1);

    auto in = inbox(4);
    CHECK(b->receive_batch(in.all()).value() == 0);
    w.clk.advance_ms(49);
    CHECK(b->receive_batch(in.all()).value() == 0);
    w.clk.advance_ms(1);
    CHECK(b->receive_batch(in.all()).value() == 1);

    // The arrival time is the moment the delay ran out, exactly.
    CHECK(in.records[0].received_at_ns == 1'000'000'000 + 50'000'000);
}

CNET_IO_TEST("cnet - the simulated arrival callback fires when a held datagram comes due")
{
    auto w = world(link_conditions{.latency_ms = 50});
    auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
    auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

    auto in = inbox(4);
    auto received = isize(0);
    b->on_arrival([&](datagram_socket& s) { received += s.receive_batch(in.all()).value(); });

    send_numbered(*a, b->local(), 1);

    // Pumped until the datagram is held and a timer is waiting for it; nothing is due yet.
    for (auto i = 0; i < 10; ++i)
        (void)cc::thread_pump_all();
    CHECK(received == 0);

    w.clk.advance_ms(50);
    CHECK(pump_until([&] { return received == 1; }));

    b->on_arrival({});
}

CNET_IO_TEST("cnet - simulated loss drops the share of datagrams it says")
{
    // 400 datagrams at a quarter loss: the binomial's standard deviation is under 9, so 250..350 is over five of them.
    for (auto const seed : {u64(1), u64(11), u64(12345)})
    {
        auto w = world(link_conditions{.loss_probability = 0.25f, .seed = seed});
        auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
        auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

        send_numbered(*a, b->local(), 400);

        auto in = inbox(512, 4);
        auto const arrived = b->receive_batch(in.all()).value();
        CHECK(arrived >= 250);
        CHECK(arrived <= 350);
    }
}

CNET_IO_TEST("cnet - simulated loss replays from its seed")
{
    auto const count_arrivals = [](u64 seed)
    {
        auto w = world(link_conditions{.loss_probability = 0.5f, .seed = seed});
        auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
        auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

        send_numbered(*a, b->local(), 40);

        auto in = inbox(64);
        return b->receive_batch(in.all()).value();
    };

    auto const first = count_arrivals(7);
    CHECK(first > 0);
    CHECK(first < 40);
    CHECK(count_arrivals(7) == first);
}

CNET_IO_TEST("cnet - simulated duplication delivers a datagram twice")
{
    auto w = world(link_conditions{.duplicate_probability = 1.0f});
    auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
    auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

    send_numbered(*a, b->local(), 3);

    auto in = inbox(8);
    CHECK(b->receive_batch(in.all()).value() == 6);
}

CNET_IO_TEST("cnet - simulated reordering lets later datagrams overtake, and arrival times stay ordered")
{
    auto w = world(link_conditions{.latency_ms = 10, .reorder_probability = 0.5f, .seed = 3});
    auto a = datagram_socket::try_create(w.t(), loopback_any_port()).value();
    auto b = datagram_socket::try_create(w.t(), loopback_any_port()).value();

    send_numbered(*a, b->local(), 20);
    w.clk.advance_ms(100);

    auto in = inbox(32);
    REQUIRE(b->receive_batch(in.all()).value() == 20);

    auto inversions = 0;
    for (isize i = 1; i < 20; ++i)
    {
        CHECK(in.records[i - 1].received_at_ns <= in.records[i].received_at_ns);
        if (in.records[i - 1].bytes()[0] > in.records[i].bytes()[0])
            ++inversions;
    }
    CHECK(inversions > 0);
}
