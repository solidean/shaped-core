#include <clean-net/address/endpoint.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

TEST("cnet - an endpoint round-trips through its text")
{
    auto const v4 = endpoint::parse("127.0.0.1:8080").value();
    CHECK(v4.address.family() == ip_family::v4);
    CHECK(v4.port == 8080);
    CHECK(v4.to_string() == "127.0.0.1:8080");

    auto const v6 = endpoint::parse("[::1]:443").value();
    CHECK(v6.address.family() == ip_family::v6);
    CHECK(v6.port == 443);
    CHECK(v6.to_string() == "[::1]:443");

    auto const scoped = endpoint::parse("[fe80::1%3]:53").value();
    CHECK(scoped.address.scope_id() == 3);
    CHECK(scoped.to_string() == "[fe80::1%3]:53");
}

TEST("cnet - an unbracketed IPv6 endpoint is refused, which is what brackets are for")
{
    CHECK(!endpoint::parse("::1:443").has_value());
    CHECK(!endpoint::parse("2001:db8::1:80").has_value());
    CHECK(endpoint::parse("[2001:db8::1]:80").has_value());
}

TEST("cnet - a port is required and bounded")
{
    CHECK(!endpoint::parse("127.0.0.1").has_value());
    CHECK(!endpoint::parse("127.0.0.1:").has_value());
    CHECK(!endpoint::parse("127.0.0.1:65536").has_value());
    CHECK(!endpoint::parse("127.0.0.1:-1").has_value());
    CHECK(!endpoint::parse("127.0.0.1:http").has_value());
    CHECK(!endpoint::parse("[::1]443").has_value());
    CHECK(!endpoint::parse("").has_value());

    // Port 0 is legal: it is what a test server binds to before asking which port it got.
    auto const any_port = endpoint::parse("127.0.0.1:0").value();
    CHECK(any_port.port == 0);
    CHECK(any_port.is_valid());
}

TEST("cnet - a name is not an endpoint")
{
    // Resolving a name needs the OS and can block, so it is cnet::resolve's job rather than a parser's.
    CHECK(!endpoint::parse("example.com:80").has_value());
    CHECK(!endpoint::parse("localhost:80").has_value());
}
