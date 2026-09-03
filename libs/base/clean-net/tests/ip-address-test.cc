#include <clean-net/address/ip_address.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

namespace
{
/// Parse, then print, then parse again -- what "canonical" has to mean.
cc::string canonical(cc::string_view text)
{
    auto const a = ip_address::parse(text);
    CHECK(a.has_value());
    auto const printed = a.value().to_string();

    auto const again = ip_address::parse(printed);
    CHECK(again.has_value());
    CHECK(again.value() == a.value());
    return printed;
}
} // namespace

TEST("cnet - a default address is not a place")
{
    auto const a = ip_address();
    CHECK(!a.is_valid());
    CHECK(a.family() == ip_family::none);
    CHECK(a.octets().empty());
    CHECK(a.to_string() == "");
    CHECK(!a.is_unspecified()); // nowhere is not the same as "every interface"
}

TEST("cnet - IPv4 parses and round-trips")
{
    CHECK(canonical("0.0.0.0") == "0.0.0.0");
    CHECK(canonical("127.0.0.1") == "127.0.0.1");
    CHECK(canonical("255.255.255.255") == "255.255.255.255");
    CHECK(canonical("192.168.1.10") == "192.168.1.10");

    auto const a = ip_address::parse("1.2.3.4").value();
    CHECK(a.family() == ip_family::v4);
    CHECK(a.octets().size() == 4);
    CHECK(a.octets()[0] == 1);
    CHECK(a.octets()[3] == 4);
}

TEST("cnet - a leading zero in IPv4 is refused rather than read as octal")
{
    // Resolvers disagree about `010`, and the disagreement is how an address allowlist gets walked past.
    CHECK(!ip_address::parse("010.0.0.1").has_value());
    CHECK(!ip_address::parse("1.2.3.04").has_value());
    CHECK(ip_address::parse("0.0.0.0").has_value()); // a bare zero is still a zero
}

TEST("cnet - malformed IPv4 is refused")
{
    CHECK(!ip_address::parse("1.2.3").has_value());
    CHECK(!ip_address::parse("1.2.3.4.5").has_value());
    CHECK(!ip_address::parse("1.2.3.256").has_value());
    CHECK(!ip_address::parse("1.2.3.").has_value());
    CHECK(!ip_address::parse(".1.2.3").has_value());
    CHECK(!ip_address::parse("1.2.3.x").has_value());
    CHECK(!ip_address::parse("").has_value());
}

TEST("cnet - IPv6 parses in every legal spelling")
{
    CHECK(ip_address::parse("::").value().is_unspecified());
    CHECK(ip_address::parse("::1").value().is_loopback());
    CHECK(ip_address::parse("2001:db8:0:0:0:0:0:1").has_value());
    CHECK(ip_address::parse("2001:db8::1").has_value());
    CHECK(ip_address::parse("fe80::1").has_value());
    CHECK(ip_address::parse("::ffff:192.168.0.1").has_value());
    CHECK(ip_address::parse("64:ff9b::1.2.3.4").has_value());

    // The same address written three ways is one address.
    auto const a = ip_address::parse("2001:0db8:0000:0000:0000:0000:0000:0001").value();
    auto const b = ip_address::parse("2001:db8::1").value();
    auto const c = ip_address::parse("2001:DB8:0:0:0:0:0:1").value();
    CHECK(a == b);
    CHECK(b == c);
}

TEST("cnet - IPv6 printing follows RFC 5952")
{
    CHECK(canonical("2001:0db8:0000:0000:0000:0000:0000:0001") == "2001:db8::1");
    CHECK(canonical("::") == "::");
    CHECK(canonical("::1") == "::1");
    CHECK(canonical("1::") == "1::");
    CHECK(canonical("2001:DB8::1") == "2001:db8::1"); // lower case

    // The longest run wins, and the leftmost of two equal runs.
    CHECK(canonical("2001:0:0:1:0:0:0:1") == "2001:0:0:1::1");
    CHECK(canonical("1:0:0:1:0:0:1:1") == "1::1:0:0:1:1");

    // A single zero group is written out: `::` for one group saves nothing and costs a reader.
    CHECK(canonical("1:2:3:4:5:6:0:8") == "1:2:3:4:5:6:0:8");

    // A v4-mapped address is written in mixed notation.
    CHECK(canonical("::ffff:192.168.0.1") == "::ffff:192.168.0.1");
    CHECK(canonical("::ffff:0:0") == "::ffff:0.0.0.0");
}

TEST("cnet - malformed IPv6 is refused")
{
    CHECK(!ip_address::parse(":::").has_value());
    CHECK(!ip_address::parse("1::2::3").has_value());           // two elisions is an ambiguous count
    CHECK(!ip_address::parse(":1").has_value());                // a lone leading colon
    CHECK(!ip_address::parse("1:").has_value());                // a lone trailing colon
    CHECK(!ip_address::parse("1:2:3:4:5:6:7").has_value());     // too few, with no elision
    CHECK(!ip_address::parse("1:2:3:4:5:6:7:8:9").has_value()); // too many
    CHECK(!ip_address::parse("1:2:3:4:5:6:7:8::").has_value()); // `::` must stand for at least one group
    CHECK(!ip_address::parse("12345::1").has_value());          // a group is at most four hex digits
    CHECK(!ip_address::parse("::g").has_value());
    CHECK(!ip_address::parse("[::1]").has_value()); // brackets belong to the endpoint grammar
}

TEST("cnet - a scope id is part of the address")
{
    auto const a = ip_address::parse("fe80::1%3").value();
    CHECK(a.scope_id() == 3);
    CHECK(a.is_link_local());
    CHECK(a.to_string() == "fe80::1%3");

    // Two interfaces can carry the same link-local address, so the scope is what separates them.
    auto const b = ip_address::parse("fe80::1%4").value();
    CHECK(a != b);
    CHECK(ip_address::parse("fe80::1").value() != a);

    CHECK(!ip_address::parse("fe80::1%").has_value());
    CHECK(!ip_address::parse("fe80::1%eth0").has_value()); // a named interface needs the OS to resolve
    CHECK(!ip_address::parse("127.0.0.1%3").has_value());  // a scope on IPv4 is meaningless
}

TEST("cnet - the well-known addresses classify")
{
    CHECK(ip_address::any(ip_family::v4).to_string() == "0.0.0.0");
    CHECK(ip_address::any(ip_family::v6).to_string() == "::");
    CHECK(ip_address::loopback(ip_family::v4).to_string() == "127.0.0.1");
    CHECK(ip_address::loopback(ip_family::v6).to_string() == "::1");

    CHECK(ip_address::any(ip_family::v4).is_unspecified());
    CHECK(ip_address::loopback(ip_family::v4).is_loopback());
    CHECK(ip_address::parse("127.9.9.9").value().is_loopback()); // the whole 127/8, not just .0.0.1

    CHECK(ip_address::parse("224.0.0.1").value().is_multicast());
    CHECK(ip_address::parse("ff02::1").value().is_multicast());
    CHECK(!ip_address::parse("223.0.0.1").value().is_multicast());

    CHECK(ip_address::parse("169.254.1.1").value().is_link_local());
    CHECK(ip_address::parse("fe80::1").value().is_link_local());
    CHECK(ip_address::parse("febf::1").value().is_link_local()); // fe80::/10 reaches febf
    CHECK(!ip_address::parse("fec0::1").value().is_link_local());
}

TEST("cnet - a v4-mapped address is not the v4 address it carries")
{
    auto const mapped = ip_address::parse("::ffff:1.2.3.4").value();
    auto const plain = ip_address::parse("1.2.3.4").value();

    CHECK(mapped.is_v4_mapped());
    CHECK(!plain.is_v4_mapped());
    CHECK(mapped.family() == ip_family::v6);

    // They reach the same machine and compare unequal, which is the trap worth pinning.
    CHECK(mapped != plain);
}
