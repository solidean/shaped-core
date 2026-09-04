#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-net/fwd.hh>

/// Which of the two address families this is, or neither.
enum class cnet::ip_family : cnet::u8
{
    /// A default-constructed address, which is not a place and cannot be connected to.
    none,
    v4,
    v6,
};

/// One IP address, plus the scope id an IPv6 link-local address needs to be usable.
///
/// A value type with no OS types in it: `sockaddr` is a backend detail and never appears in a clean-net header.
///
/// The scope id is part of the address rather than a decoration on it.
/// `fe80::1` alone does not say which interface, and two machines on two networks can both have it -- which is why
/// an address without its scope is not routable and comparing one against another is meaningless.
struct cnet::ip_address
{
    /// The unusable address: `family()` is `none`.
    ip_address() = default;

    /// Four octets in network order.
    [[nodiscard]] static ip_address from_v4(cc::span<u8 const> octets);

    /// Sixteen octets in network order, plus the interface scope for a link-local address.
    [[nodiscard]] static ip_address from_v6(cc::span<u8 const> octets, u32 scope_id = 0);

    /// `0.0.0.0` / `::` -- what a listener binds to when it means "every interface".
    [[nodiscard]] static ip_address any(ip_family family);

    /// `127.0.0.1` / `::1`.
    [[nodiscard]] static ip_address loopback(ip_family family);

    /// Parse a textual address.
    ///
    /// IPv4 is a dotted quad, and **a leading zero is rejected rather than read as octal**: `010.0.0.1` is
    /// ambiguous, different resolvers disagree about it, and the disagreement is how an allowlist gets walked past.
    /// IPv6 accepts the `::` run, a trailing dotted quad, and a `%scope` suffix, and is otherwise RFC 4291.
    /// The scope is **numeric only**: an interface may be named rather than numbered on some platforms, and
    /// resolving a name needs the OS.
    /// Brackets are NOT accepted here -- they belong to the endpoint grammar, not the address one.
    [[nodiscard]] static cc::optional<ip_address> parse(cc::string_view text);

    [[nodiscard]] ip_family family() const { return _family; }
    [[nodiscard]] bool is_valid() const { return _family != ip_family::none; }

    /// Four octets for v4, sixteen for v6, empty otherwise.
    /// Network order.
    [[nodiscard]] cc::span<u8 const> octets() const;

    /// The interface this address is scoped to; 0 when it is not scoped.
    /// Only ever non-zero for IPv6.
    [[nodiscard]] u32 scope_id() const { return _scope_id; }

    /// `0.0.0.0` or `::` -- the "any interface" address, which is a bind target and never a destination.
    [[nodiscard]] bool is_unspecified() const;

    /// `127.0.0.0/8` or `::1`.
    [[nodiscard]] bool is_loopback() const;

    /// `224.0.0.0/4` or `ff00::/8`.
    [[nodiscard]] bool is_multicast() const;

    /// `169.254.0.0/16` or `fe80::/10` -- the addresses that need a scope id to mean anything.
    [[nodiscard]] bool is_link_local() const;

    /// `::ffff:0:0/96`, an IPv4 address carried in an IPv6 one.
    /// Worth asking about: a v4-mapped address compares unequal to the v4 address it carries, and reaches the same
    /// machine.
    [[nodiscard]] bool is_v4_mapped() const;

    /// The canonical text (RFC 5952 for IPv6): lower case, leading zeros dropped, the longest run of zero groups
    /// replaced by `::`, and a v4-mapped address written in mixed notation.
    ///
    /// Round-trips through parse, and two addresses that are equal produce the same text.
    [[nodiscard]] cc::string to_string() const;

    /// The ADL hook cc::to_debug_string and cc::format find.
    [[nodiscard]] friend cc::string to_string(ip_address const& a) { return a.to_string(); }

    /// Byte-wise, including the scope id: `fe80::1%3` and `fe80::1%4` are different places.
    /// The unused octets of a v4 address are always zero, so this compares what it looks like it compares.
    [[nodiscard]] friend bool operator==(ip_address const&, ip_address const&) = default;

private:
    u8 _octets[16] = {};
    ip_family _family = ip_family::none;
    u32 _scope_id = 0;
};
