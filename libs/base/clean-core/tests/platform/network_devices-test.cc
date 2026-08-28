#include <clean-core/platform/network_devices.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Which interfaces a machine has is not something a test can know.
// What it pins is that an interface has an id a sampler can key on, that the counters are monotone, and that an
// interface which is not there fails cleanly rather than differencing two unrelated counters.

TEST("cc network_devices - every interface has an id to key a sampler on")
{
    for (auto const& n : cc::enumerate_network_interfaces())
    {
        CHECK(!n.id.empty());
        if (n.link_speed_bps.has_value())
            CHECK(n.link_speed_bps.value() > 0);
    }
}

TEST("cc network_devices - interface ids are unique")
{
    // A repeated id would make two samplers silently share one interface's counters.
    auto const interfaces = cc::enumerate_network_interfaces();
    for (isize i = 0; i < interfaces.size(); ++i)
        for (isize j = i + 1; j < interfaces.size(); ++j)
            CHECK(interfaces[i].id != interfaces[j].id);
}

TEST("cc network_devices - every enumerated interface answers for its own counters")
{
    // Enumeration and lookup must agree: an id that comes out of one and is rejected by the other is the bug that makes
    // a dashboard show a permanently empty panel.
    for (auto const& n : cc::enumerate_network_interfaces())
    {
        auto const counters = cc::read_net_counters(n.id);
        CHECK(counters.has_value());
    }
}

TEST("cc network_devices - counters are monotone")
{
    auto const interfaces = cc::enumerate_network_interfaces();
    if (interfaces.empty())
        return;

    auto first = cc::read_net_counters(interfaces[0].id);
    if (first.has_error())
        return;

    auto second = cc::read_net_counters(interfaces[0].id);
    REQUIRE(second.has_value());

    CHECK(second.value().bytes_sent >= first.value().bytes_sent);
    CHECK(second.value().bytes_received >= first.value().bytes_received);
    CHECK(second.value().packets_sent >= first.value().packets_sent);
    CHECK(second.value().packets_received >= first.value().packets_received);
}

TEST("cc network_devices - a sampled rate is non-negative")
{
    auto const interfaces = cc::enumerate_network_interfaces();
    if (interfaces.empty() || !cc::net_traffic_sampler::is_supported())
        return;

    auto sampler = cc::net_traffic_sampler(interfaces[0].id);
    auto const rate = sampler.sample();
    if (rate.has_error())
        return;

    CHECK(rate.value().sent_bytes_per_sec >= 0);
    CHECK(rate.value().received_bytes_per_sec >= 0);
    CHECK(rate.value().sent_packets_per_sec >= 0);
    CHECK(rate.value().received_packets_per_sec >= 0);
}

TEST("cc network_devices - an interface that is not there reports absence")
{
    auto const missing = cc::string_view("cc-no-such-interface-7f3a");

    auto const counters = cc::read_net_counters(missing);
    REQUIRE(counters.has_error());
    CHECK(counters.error().status == cc::query_status::unsupported);

    auto sampler = cc::net_traffic_sampler(missing);
    CHECK(sampler.sample().has_error());
}
