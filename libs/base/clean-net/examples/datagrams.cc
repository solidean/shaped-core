#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/datagram.hh>
#include <clean-net/transport/simulated_transport.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Datagrams, twice: an echo server over loopback, then the same calls over a bad link that exists only in this process.
//
// Nothing here waits for a packet with a sleep.
// The echo server is called back when something arrives; the client polls once per "frame", the way a game would.
// Both io_systems are unthreaded, so this example drives them with `cc::thread_pump_all()` -- a frame loop would too.

namespace
{
/// Records over one buffer, set up once: a receive fills them and never shrinks the room they were given.
struct records
{
    cc::vector<byte> storage;
    cc::vector<cnet::datagram> all;

    explicit records(isize count, isize slot = 1500)
    {
        storage.resize_to_defaulted(count * slot);
        all.resize_to_defaulted(count);
        for (isize i = 0; i < count; ++i)
            all[i].buffer = cc::span<byte>(storage).subspan(i * slot).first_n(slot);
    }
};

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}

[[nodiscard]] cc::string_view text_of(cnet::datagram const& d)
{
    return cc::string_view(reinterpret_cast<char const*>(d.bytes().data()), d.size);
}

void echo_over_loopback()
{
    auto io = cnet::io_system::create({.unthreaded = true});
    auto const here = cnet::endpoint(cnet::ip_address::loopback(cnet::ip_family::v4), 0);

    auto server = cnet::datagram_socket::try_create(*io, here);
    if (server.has_error())
    {
        cc::println("no datagram sockets here: {}", server.error().message); // wasm
        return;
    }

    // The server is told when datagrams are waiting, drains them, and answers each one in a single batch.
    auto server_in = records(16);
    server.value()->on_arrival(
        [&](cnet::datagram_socket& s)
        {
            auto const n = s.receive_batch(server_in.all).value();
            auto replies = cc::vector<cnet::outgoing_datagram>();
            for (auto const& d : cc::span<cnet::datagram>(server_in.all).first_n(n))
                replies.push_back({.peer = d.peer, .bytes = d.bytes()});
            (void)s.send_batch(replies);
        });

    auto client = cnet::datagram_socket::create(*io, here);
    auto const words = cc::vector<cc::string_view>{"ping", "pong", "pang"};
    auto out = cc::vector<cnet::outgoing_datagram>();
    for (auto w : words)
        out.push_back({.peer = server.value()->local(), .bytes = bytes_of(w)});
    cc::println("client sent {} datagrams to {}", client->send_batch(out).value(), server.value()->local());

    // The client polls once per frame, and a frame with nothing waiting costs one syscall.
    auto client_in = records(16);
    auto echoed = isize(0);
    for (auto frame = 0; frame < 1000 && echoed < 3; ++frame)
    {
        (void)cc::thread_pump_all();
        auto const n = client->receive_batch(client_in.all).value();
        for (auto const& d : cc::span<cnet::datagram>(client_in.all).first_n(n))
            cc::println("  frame {}: \"{}\" came back from {}", frame, text_of(d), d.peer);
        echoed += n;
        cc::this_thread_yield();
    }

    server.value()->on_arrival({});
}

void over_a_bad_link()
{
    // A link with 40 ms of latency and a quarter of the datagrams lost, on a clock this example moves by hand.
    // The same seed loses the same datagrams on every run; over eight datagrams the share it loses is far from a quarter.
    auto clk = cnet::manual_clock(0);
    auto io = cnet::io_system::create({.unthreaded = true, .time_source = &clk});
    auto net = cnet::virtual_network(*io);
    auto link
        = cnet::simulated_transport(*io, net, {.latency_ms = 40, .jitter_ms = 5, .loss_probability = 0.25f, .seed = 11});

    auto const here = cnet::endpoint(cnet::ip_address::loopback(cnet::ip_family::v4), 0);
    auto a = cnet::datagram_socket::try_create(link, here).value();
    auto b = cnet::datagram_socket::try_create(link, here).value();

    auto const words = cc::vector<cc::string_view>{"one", "two", "three", "four", "five", "six", "seven", "eight"};
    auto out = cc::vector<cnet::outgoing_datagram>();
    for (auto w : words)
        out.push_back({.peer = b->local(), .bytes = bytes_of(w)});
    (void)a->send_batch(out);

    auto in = records(16);
    for (auto ms = 0; ms <= 60; ms += 10)
    {
        auto const n = b->receive_batch(in.all).value();
        cc::println("at {:2} ms: {} arrived", ms, n);
        for (auto const& d : cc::span<cnet::datagram>(in.all).first_n(n))
            cc::println("    \"{}\", stamped {} ms", text_of(d), d.received_at_ns / 1'000'000);
        clk.advance_ms(10);
    }
}
} // namespace

EXAMPLE("clean-net/datagrams")
{
    cc::println("-- an echo server over loopback --");
    echo_over_loopback();

    cc::println("");
    cc::println("-- the same calls over a simulated link --");
    over_a_bad_link();
}
