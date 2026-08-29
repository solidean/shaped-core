#include <clean-core/platform/network_devices.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Which interfaces a machine has is not something a test can know.
// What it pins is that an interface has an id a sampler can key on, that the counters are monotone, and that an
// interface which is not there fails cleanly rather than differencing two unrelated counters.

TEST("cc network_devices - every interface has an id to key a sampler on")
{
    // Counted rather than checked per interface, so the test asserts the property itself and still says something on a
    // platform that enumerates nothing.
    auto without_id = 0;
    for (auto const& n : cc::enumerate_network_interfaces())
    {
        if (n.id.empty())
            ++without_id;
        if (n.link_speed_bps.has_value())
            CHECK(n.link_speed_bps.value() > 0);
    }

    CHECK(without_id == 0);
}

TEST("cc network_devices - interface ids are unique")
{
    auto const interfaces = cc::enumerate_network_interfaces();

    auto repeats = 0;
    for (isize i = 0; i < interfaces.size(); ++i)
        for (isize j = i + 1; j < interfaces.size(); ++j)
            if (interfaces[i].id == interfaces[j].id)
                ++repeats;

    // A repeated id would make two samplers silently share one interface's counters.
    CHECK(repeats == 0);
}

TEST("cc network_devices - every enumerated interface answers for its own counters")
{
    auto unanswered = 0;
    for (auto const& n : cc::enumerate_network_interfaces())
        if (cc::read_net_counters(n.id).has_error())
            ++unanswered;

    // Enumeration and lookup must agree: an id that comes out of one and is rejected by the other is the bug that makes
    // a dashboard show a permanently empty panel.
    CHECK(unanswered == 0);
}

TEST("cc network_devices - counters are monotone")
{
    auto const interfaces = cc::enumerate_network_interfaces();
    if (interfaces.empty())
    {
        // Nothing to difference, and nothing invented in its place: a lookup here fails rather than returning zeroes.
        CHECK(cc::read_net_counters("cc-no-such-interface-7f3a").has_error());
        return;
    }

    auto first = cc::read_net_counters(interfaces[0].id);
    if (first.has_error())
    {
        CHECK(!first.error().detail.empty());
        return;
    }

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
    {
        CHECK(cc::read_net_counters("cc-no-such-interface-7f3a").has_error());
        return;
    }

    auto sampler = cc::net_traffic_sampler(interfaces[0].id);
    auto const rate = sampler.sample();
    if (rate.has_error())
    {
        CHECK(!rate.error().detail.empty());
        return;
    }

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
