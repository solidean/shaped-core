#pragma once

#include <clean-core/string/string_view.hh>
#include <clean-net/address/resolver.hh>
#include <clean-net/transport/stream.hh>

/// Connecting to a NAME rather than to an address, which is what almost every caller actually has.
///
/// The two steps -- resolve, then connect -- are one operation here, because a caller that had to write them out
/// would also have to decide what to do with four addresses, and the honest answer to that is a race.

/// What connecting to a host is allowed to take.
struct cnet::connect_options
{
    /// One budget for the whole thing: the resolve and every connection attempt spend it together.
    ///
    /// A per-step timeout would let a connect to a four-address host take four times what the caller asked for, which
    /// is the difference between a bounded request and one that is merely bounded per step.
    deadline timeout = deadline::after_secs(30);

    tcp_options socket;

    /// Which family to ask the resolver for.
    /// Racing is the default, and the reason this call exists.
    address_family_preference family = address_family_preference::race;

    /// How long one attempt gets on its own before the next is started alongside it.
    ///
    /// RFC 8305 calls this the Connection Attempt Delay and recommends 250 ms.
    /// It is not a timeout: the earlier attempt keeps running, and whichever answers first wins.
    i32 attempt_delay_ms = 250;
};

namespace cnet
{
/// Resolve `host` and connect to it, racing the addresses (RFC 8305, "happy eyeballs").
///
/// **A machine with broken IPv6 routing loses milliseconds rather than a connect timeout**, which is the whole point:
/// no per-family preference can detect a route that is advertised and does not work, and only trying both can.
/// Attempts start staggered by `attempt_delay_ms` and the first success wins; the losers are cancelled, including one
/// that succeeded a moment too late, whose connection is closed rather than leaked.
///
/// The race runs under a child of `token` and every attempt is started with it, so cancelling the caller's token
/// cancels the whole race while finishing the race cancels only its own attempts.
///
/// Fails with `name_not_resolved` if the host has no address, and with the FIRST attempt's failure if every attempt
/// failed -- the first rather than the last, because it is the one about the address the OS thought best.
[[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> connect_to_host(transport& t,
                                                                                  resolver& r,
                                                                                  cc::string_view host,
                                                                                  i32 port,
                                                                                  connect_options const& options = {},
                                                                                  cancel_token const& token = {});

/// The same over the platform's own sockets.
[[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> connect_to_host(io_system& io,
                                                                                  resolver& r,
                                                                                  cc::string_view host,
                                                                                  i32 port,
                                                                                  connect_options const& options = {},
                                                                                  cancel_token const& token = {});
} // namespace cnet
