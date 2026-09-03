#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/fwd.hh>

/// Turning an already-upgraded connection into a `cnet::websocket`.
///
/// Both ends need this and neither end's handshake belongs in the other's file: a client parses a 101 and a server
/// writes one, and what they share is only the moment after.

namespace cnet::impl
{
/// What an already-upgraded connection needs to become a WebSocket.
struct websocket_adoption
{
    cc::shared_ptr<stream_connection> connection;

    /// Which side masks, which the protocol fixes and neither end may get wrong.
    bool is_client = true;

    cc::string negotiated_protocol;

    /// Whatever arrived after the handshake's last byte, which must be handed over rather than dropped: a peer is
    /// allowed to put its first message in the same packet as the end of the handshake, and a reader that starts
    /// from the socket loses it.
    cc::vector<byte> leftover;

    isize max_message_bytes = 8 * 1024 * 1024;
    i32 ping_interval_ms = 30'000;
    i32 pong_timeout_ms = 10'000;

    cancel_token token;
};

/// Take over a connection that has finished its handshake.
///
/// `io` is where the keepalive's timers go, which is the one thing a WebSocket needs beyond its connection.
[[nodiscard]] cc::shared_ptr<websocket> adopt_websocket(io_system& io, websocket_adoption adoption);
} // namespace cnet::impl
