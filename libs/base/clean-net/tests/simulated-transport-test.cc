#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/transport/simulated_transport.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// A bad network, made reproducible.
// Everything here runs over a virtual_network with a manual clock, so a 200 ms link costs no wall-clock time at all
// and a seeded failure replays exactly.

namespace
{
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

[[nodiscard]] endpoint somewhere()
{
    return endpoint(ip_address::loopback(ip_family::v4), 0);
}

/// A virtual network with a simulated link over it, and the clock both are measured against.
struct sim_fixture
{
    manual_clock clk = manual_clock(0);
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<simulated_transport> link;

    explicit sim_fixture(link_conditions const& conditions)
    {
        io = io_system::create({.unthreaded = true, .time_source = &clk});
        net = cc::make_unique<virtual_network>(*io);
        link = cc::make_unique<simulated_transport>(*io, *net, conditions);
    }
};
} // namespace

TEST("cnet - a simulated link with no conditions set is the transport underneath")
{
    auto fixture = sim_fixture({});

    auto listener = stream_listener::try_create(*fixture.link, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(*fixture.link, listener->local());

    // Nothing was configured, so nothing waits: this is the property that makes a passing test over a simulated link
    // worth anything at all.
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(accepted->try_error() == nullptr);

    auto const greeting = cc::string_view("straight through");
    CHECK(pump_until([&] { return connected->value()->send(bytes_of(greeting))->is_ready(); }));

    byte inbox[64] = {};
    auto received = accepted->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->value() == greeting.size());
}

TEST("cnet - latency is paid on the injected clock rather than in wall-clock time")
{
    auto fixture = sim_fixture({.latency_ms = 200});

    auto listener = stream_listener::try_create(*fixture.link, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(*fixture.link, listener->local());

    // The connect is waiting on a timer, not on anything the virtual network has to do.
    // Two operations are pending: that timer, and the accept nobody has connected to yet.
    CHECK(!connected->is_ready());
    CHECK(pump_until([&] { return fixture.io->pending_count() == 2; }));

    fixture.clk.advance_ms(200);
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);
    CHECK(pump_until([&] { return accepted->is_ready(); }));

    // And every read pays it too.
    auto const greeting = cc::string_view("late but here");
    auto sent = connected->value()->send(bytes_of(greeting));
    fixture.clk.advance_ms(200);
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);

    byte inbox[64] = {};
    auto received = accepted->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(!received->is_ready());
    fixture.clk.advance_ms(200);
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->value() == greeting.size());
}

TEST("cnet - a link cut after N bytes kills the connection mid-stream")
{
    auto fixture = sim_fixture({.reset_after_bytes = 8});

    auto listener = stream_listener::try_create(*fixture.link, somewhere()).value();
    auto accepted = listener->accept();
    auto connected = tcp_connect(*fixture.link, listener->local());
    CHECK(pump_until([&] { return accepted->is_ready() && connected->is_ready(); }));

    auto const& client = connected->value();
    auto const& peer = accepted->value();

    // Under the budget, everything is ordinary.
    CHECK(pump_until([&] { return client->send(bytes_of("1234"))->is_ready(); }));

    byte inbox[64] = {};
    auto first = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return first->is_ready(); }));
    CHECK(first->try_error() == nullptr);
    CHECK(first->value() == 4);

    // The read that crosses it is the one that dies -- the failure that never reproduces on loopback.
    CHECK(pump_until([&] { return client->send(bytes_of("56789"))->is_ready(); }));
    auto second = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return second->is_ready(); }));
    CHECK(second->try_error() != nullptr);
    CHECK(!peer->is_open());

    // And it stays dead, rather than working again on the next call.
    auto third = peer->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return third->is_ready(); }));
    CHECK(third->try_error() != nullptr);
}

TEST("cnet - a link that loses everything refuses the connection")
{
    auto fixture = sim_fixture({.loss_probability = 1.0f});

    auto listener = stream_listener::try_create(*fixture.link, somewhere()).value();
    auto connected = tcp_connect(*fixture.link, listener->local());

    CHECK(connected->is_ready());
    CHECK(connected->try_error() != nullptr);
}

TEST("cnet - the same seed loses the same operations")
{
    // Two links, same seed, same coin flips: a failing run replays from the seed and the conditions alone.
    auto const conditions = link_conditions{.loss_probability = 0.5f, .seed = 12345};

    auto outcomes = cc::vector<cc::vector<bool>>();
    for (i32 run = 0; run < 2; ++run)
    {
        auto fixture = sim_fixture(conditions);
        auto listener = stream_listener::try_create(*fixture.link, somewhere()).value();

        auto attempts = cc::vector<bool>();
        for (i32 i = 0; i < 12; ++i)
        {
            auto connected = tcp_connect(*fixture.link, listener->local());
            CHECK(pump_until([&] { return connected->is_ready(); }));
            attempts.push_back(connected->try_error() == nullptr);
        }
        outcomes.push_back(cc::move(attempts));
    }

    CHECK(outcomes[0].size() == outcomes[1].size());
    for (isize i = 0; i < outcomes[0].size(); ++i)
        CHECK(outcomes[0][i] == outcomes[1][i]);

    // And the coin is actually being flipped, rather than every attempt going the same way.
    auto lost = 0;
    for (auto ok : outcomes[0])
        if (!ok)
            ++lost;
    CHECK(lost > 0);
    CHECK(lost < 12);
}
