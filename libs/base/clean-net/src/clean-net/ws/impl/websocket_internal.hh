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
/// Take over a connection that has finished its handshake.
///
/// `leftover` is whatever arrived after the handshake's last byte and must be handed over rather than dropped: a peer
/// is allowed to put its first message in the same packet as the end of the handshake, and a reader that starts from
/// the socket loses it.
///
/// `is_client` decides which side masks, which the protocol fixes and neither end may get wrong.
[[nodiscard]] cc::shared_ptr<websocket> adopt_websocket(cc::shared_ptr<stream_connection> connection,
                                                        bool is_client,
                                                        cc::string negotiated_protocol,
                                                        cc::vector<byte> leftover,
                                                        isize max_message_bytes,
                                                        cancel_token const& token);
} // namespace cnet::impl
