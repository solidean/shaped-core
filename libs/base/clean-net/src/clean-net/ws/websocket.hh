#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/address/resolver.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/tls/tls.hh>
#include <clean-net/transport/stream.hh>

/// WebSocket, over whatever connection it was handed.
///
/// **A message rather than a stream**, which is the whole reason the protocol exists: TCP has no message boundaries
/// and a browser needs them, so this hands back whole messages and hides the frames they arrived in.
///
/// **Ping, pong and close are answered here**, not by the caller.
/// A ping that goes unanswered is a connection some proxy will drop, and a close that goes unacknowledged is one
/// that ends by timeout rather than agreement -- neither is a decision worth making per application.
///
/// Like TLS, this is a wrapper: it takes a connection and speaks a protocol over it, so it works over a socket, over
/// a virtual network with no socket at all, and over a simulated link that drops records.

/// One message, whole.
struct cnet::websocket_message
{
    /// Text messages are UTF-8 by the protocol's own rule; this does not check, and neither does anything below it.
    bool is_text = true;

    cc::vector<byte> data;

    [[nodiscard]] cc::string_view text() const
    {
        return cc::string_view(reinterpret_cast<char const*>(data.data()), data.size());
    }
};

/// What a connection is opened with.
struct cnet::websocket_options
{
    /// Subprotocols to offer, in preference order.
    /// The one the server picked is `websocket::protocol()`.
    cc::vector<cc::string> protocols;

    /// The largest message that will be reassembled, in bytes.
    ///
    /// A peer can otherwise send fragments forever and never set the final bit, which is a memory limit reached by a
    /// message that never arrives.
    isize max_message_bytes = 8 * 1024 * 1024;

    /// How often an IDLE connection is pinged, and how long the pong may take.
    ///
    /// **This is what turns a dead connection into a failure instead of a wait.**
    /// A peer whose machine vanished sends no FIN, so a `receive` on that connection is indistinguishable from one on
    /// a quiet connection -- and `receive` defaults to no deadline at all.
    /// It is also what keeps a long-lived connection alive through a proxy that drops idle ones.
    ///
    /// Idle means idle: a connection carrying messages is never pinged, since the messages already prove it is there.
    /// 0 turns keepalives off.
    i32 ping_interval_ms = 30'000;

    /// How long a pong may take before the connection is treated as dead.
    /// The receive then fails with `timed_out` rather than waiting for a peer that is not coming back.
    i32 pong_timeout_ms = 10'000;

    /// What TLS is set up with, for a `wss` URL.
    tls_options tls;
};

/// An open WebSocket.
class cnet::websocket
{
public:
    /// Send one text message.
    /// The bytes are copied, since a frame is built from them and the caller's buffer is theirs again on return.
    [[nodiscard]] cc::shared_async<cc::unit> send_text(cc::string_view text,
                                                       deadline d = deadline::after_secs(30),
                                                       cancel_token const& token = {});

    [[nodiscard]] cc::shared_async<cc::unit> send_binary(cc::span<byte const> data,
                                                         deadline d = deadline::after_secs(30),
                                                         cancel_token const& token = {});

    /// The next whole message.
    ///
    /// Fails with `connection_closed` once the peer has closed, which is the ordinary end of a WebSocket rather than
    /// a failure -- a caller loops on this until it does.
    /// Two receives at once are a caller error: the second would take the message the first was promised.
    [[nodiscard]] cc::shared_async<websocket_message> receive(deadline d = deadline::never(),
                                                              cancel_token const& token = {});

    /// Send a close frame and stop.
    ///
    /// The code is what the peer is told; 1000 means "done", and anything a caller invents belongs in 4000-4999.
    void close(u16 code = 1000, cc::string_view reason = {});

    [[nodiscard]] bool is_open() const;

    /// The subprotocol both ends agreed on, empty when none was offered or none matched.
    [[nodiscard]] cc::string_view protocol() const;

    /// The connection underneath, for its endpoints.
    [[nodiscard]] endpoint peer() const;

    explicit websocket(cc::shared_ptr<struct websocket_state> state);
    websocket(websocket const&) = delete;
    websocket& operator=(websocket const&) = delete;
    ~websocket();

private:
    cc::shared_ptr<struct websocket_state> _state;
};

namespace cnet
{
/// Open a WebSocket to `url`, which must be `ws://` or `wss://`.
///
/// Resolves, connects, upgrades over TLS where the scheme says so, and performs the handshake -- so this is the one
/// call a client needs.
/// Fails with `protocol_error` when the server answers something other than a proper 101.
[[nodiscard]] cc::shared_async<cc::shared_ptr<websocket>> websocket_connect(transport& t,
                                                                            resolver& r,
                                                                            cc::string_view url,
                                                                            websocket_options const& options = {},
                                                                            deadline d = deadline::after_secs(30),
                                                                            cancel_token const& token = {});

/// The same over the platform's own sockets.
[[nodiscard]] cc::shared_async<cc::shared_ptr<websocket>> websocket_connect(io_system& io,
                                                                            resolver& r,
                                                                            cc::string_view url,
                                                                            websocket_options const& options = {},
                                                                            deadline d = deadline::after_secs(30),
                                                                            cancel_token const& token = {});
} // namespace cnet
