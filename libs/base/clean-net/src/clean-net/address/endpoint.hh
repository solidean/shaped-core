#pragma once

#include <clean-net/address/ip_address.hh>

/// An address and a port: what a socket connects to, or listens on.
///
/// The text form brackets an IPv6 address -- `[::1]:80` -- because without brackets the last colon is ambiguous.
/// That bracket rule belongs here rather than in ip_address, which is why `ip_address::parse` refuses them.
struct cnet::endpoint
{
    ip_address address;

    /// 0 means "any free port", which is what a test server binds to and then asks which one it got.
    i32 port = 0;

    endpoint() = default;
    endpoint(ip_address a, i32 p) : address(a), port(p) {}

    /// Parse `host:port`, with an IPv6 address bracketed.
    ///
    /// The host must be an address rather than a name: resolving a name needs the OS and can block, so it is
    /// `cnet::resolve`'s job and never a constructor's.
    /// Fails on a missing port, a port outside 0-65535, and an unbracketed IPv6 address.
    [[nodiscard]] static cc::optional<endpoint> parse(cc::string_view text);

    [[nodiscard]] bool is_valid() const { return address.is_valid() && port >= 0 && port <= 0xFFFF; }

    /// The canonical text, bracketing an IPv6 address.
    [[nodiscard]] cc::string to_string() const;

    /// The ADL hook cc::to_debug_string and cc::format find.
    [[nodiscard]] friend cc::string to_string(endpoint const& e) { return e.to_string(); }

    [[nodiscard]] friend bool operator==(endpoint const&, endpoint const&) = default;
};
