#include "websocket.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/http/impl/http1.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/transport/connect.hh>
#include <clean-net/ws/impl/websocket_internal.hh>
#include <clean-net/ws/impl/ws_frame.hh>

// A message protocol over a byte stream, and the bookkeeping that turns one into the other.
//
// ONE READ AND ONE WRITE AT A TIME.
// Frames may not interleave on the wire, so sends queue: each one is framed, put in a queue, and written when the
// one before it is done.
// Reads are one at a time by contract, because a second receive would take the message the first was promised.
//
// CONTROL FRAMES ARE ANSWERED HERE.
// A ping is ponged, a close is acknowledged and ends the connection, and a pong is dropped -- none of which is a
// decision worth making per application, and all of which a peer will hold against a connection that skips them.

namespace cnet
{
namespace
{
constexpr isize k_read_chunk = 16 * 1024;

/// One frame waiting its turn on the wire.
struct outgoing_frame
{
    cc::vector<byte> bytes;
    cc::shared_async<cc::unit> promise;
    deadline d;
};

/// What the reader is in the middle of.
struct partial_message
{
    bool active = false;
    bool is_text = true;
    cc::vector<byte> data;
};
} // namespace

/// Everything one WebSocket owns.
struct websocket_state
{
    cc::shared_ptr<stream_connection> connection;
    cancel_token token;

    /// A client masks and a server does not; the protocol fixes which, and getting it wrong is the classic bug.
    bool is_client = true;

    cc::string negotiated_protocol;
    isize max_message_bytes = 8 * 1024 * 1024;

    struct data
    {
        cc::vector<byte> inbox;
        partial_message partial;

        /// Messages that finished before anybody asked for one.
        cc::vector<websocket_message> ready;

        cc::vector<outgoing_frame> outbox;
        bool writing = false;

        cc::shared_async<websocket_message> pending_receive;
        deadline receive_deadline = deadline::never();

        bool reading = false;
        bool closed = false;
        bool close_sent = false;
        cc::optional<error> fatal;

        /// Mask keys come from here, and they only have to differ rather than be unguessable.
        u32 mask_counter = 0x9E3779B9;
    };

    cc::mutex<data> state;

    cc::vector<byte> read_buffer;
};

namespace
{
void pump_reads(cc::shared_ptr<websocket_state> const& ws);
void pump_writes(cc::shared_ptr<websocket_state> const& ws);

/// Queue a frame, and start writing if nothing else is.
[[nodiscard]] cc::shared_async<cc::unit> enqueue_frame(cc::shared_ptr<websocket_state> const& ws,
                                                       impl::ws_opcode opcode,
                                                       cc::span<byte const> payload,
                                                       deadline d)
{
    auto promise = cc::make_async_manual<cc::unit>();

    auto const refused = ws->state.lock(
        [&](websocket_state::data& d_state) -> bool
        {
            if (d_state.closed || d_state.fatal.has_value())
                return true;

            auto frame = outgoing_frame();
            frame.promise = promise;
            frame.d = d;

            if (ws->is_client)
            {
                // Different every time is all this needs to be: masking is about transparent proxies, not secrecy.
                d_state.mask_counter = d_state.mask_counter * 1664525u + 1013904223u;
                auto const key = d_state.mask_counter;

                u8 const mask[4]
                    = {u8(key & 0xFF), u8((key >> 8) & 0xFF), u8((key >> 16) & 0xFF), u8((key >> 24) & 0xFF)};
                impl::write_frame(frame.bytes, opcode, payload, true, mask);
            }
            else
            {
                u8 const unused[4] = {};
                impl::write_frame(frame.bytes, opcode, payload, false, unused);
            }

            d_state.outbox.push_back(cc::move(frame));
            return false;
        });

    if (refused)
        return impl::failed_async<cc::unit>(
            {.code = error_code::connection_closed, .native_code = 0, .message = cc::string("the websocket is closed")});

    pump_writes(ws);
    return promise;
}

void pump_writes(cc::shared_ptr<websocket_state> const& ws)
{
    struct next_write
    {
        bool start = false;
        cc::span<byte const> bytes;
        deadline d = deadline::never();
    };

    auto next = ws->state.lock(
        [](websocket_state::data& d_state) -> next_write
        {
            if (d_state.writing || d_state.outbox.empty())
                return {};

            d_state.writing = true;
            auto const& front = d_state.outbox[0];
            return {.start = true, .bytes = cc::span<byte const>(front.bytes.data(), front.bytes.size()), .d = front.d};
        });

    if (!next.start)
        return;

    impl::when_ready(ws->connection->send(next.bytes, next.d, ws->token),
                     [ws](cc::shared_async<cc::unit> const& sent)
                     {
                         auto finished = ws->state.lock(
                             [&](websocket_state::data& d_state)
                             {
                                 d_state.writing = false;

                                 auto promise = d_state.outbox[0].promise;
                                 for (isize i = 1; i < d_state.outbox.size(); ++i)
                                     d_state.outbox[i - 1] = cc::move(d_state.outbox[i]);
                                 d_state.outbox.remove_back();
                                 return promise;
                             });

                         if (sent->has_error())
                             finished->push_error(sent->propagate_error());
                         else
                             finished->push_value(cc::unit{});

                         pump_writes(ws);
                     });
}

/// Answer a control frame, which is this layer's job rather than the caller's.
void handle_control(cc::shared_ptr<websocket_state> const& ws, impl::ws_opcode opcode, cc::vector<byte> payload)
{
    if (opcode == impl::ws_opcode::ping)
    {
        (void)enqueue_frame(ws, impl::ws_opcode::pong, payload, deadline::after_secs(30));
        return;
    }

    if (opcode == impl::ws_opcode::pong)
        return; // nothing here sends pings, so a pong is somebody being polite

    // A close is acknowledged once and then the connection is over.
    auto const already_sent = ws->state.lock(
        [](websocket_state::data& d_state)
        {
            auto const was = d_state.close_sent;
            d_state.close_sent = true;
            d_state.closed = true;
            return was;
        });

    if (!already_sent)
    {
        // The peer's own code is echoed back, which is what the RFC asks for and what makes a close mutual rather
        // than a hang-up.
        auto echo = cc::vector<byte>();
        if (payload.size() >= 2)
        {
            echo.push_back(payload[0]);
            echo.push_back(payload[1]);
        }
        (void)enqueue_frame(ws, impl::ws_opcode::close, echo, deadline::after_secs(5));
    }
}

/// Complete a parked receive, if there is one and something to give it.
void deliver(cc::shared_ptr<websocket_state> const& ws)
{
    struct delivery
    {
        cc::shared_async<websocket_message> promise;
        websocket_message message;
        cc::optional<error> failure;
    };

    auto ready = ws->state.lock(
        [](websocket_state::data& d_state) -> cc::optional<delivery>
        {
            if (!d_state.pending_receive.is_valid())
                return {};

            // A message that arrived first is handed over first, even if the connection has since ended: the bytes
            // are here, and the close is the next thing the caller will hear about rather than instead of them.
            if (!d_state.ready.empty())
            {
                auto out = delivery{.promise = d_state.pending_receive, .message = cc::move(d_state.ready[0])};

                for (isize i = 1; i < d_state.ready.size(); ++i)
                    d_state.ready[i - 1] = cc::move(d_state.ready[i]);
                d_state.ready.remove_back();

                d_state.pending_receive = {};
                return out;
            }

            if (d_state.fatal.has_value())
            {
                auto out = delivery{.promise = d_state.pending_receive, .failure = d_state.fatal};
                d_state.pending_receive = {};
                return out;
            }

            if (d_state.closed)
            {
                auto out = delivery{.promise = d_state.pending_receive,
                                    .failure = error{.code = error_code::connection_closed,
                                                     .native_code = 0,
                                                     .message = cc::string("the peer closed the websocket")}};
                d_state.pending_receive = {};
                return out;
            }

            return {};
        });

    if (ready.has_value())
    {
        if (ready.value().failure.has_value())
            ready.value().promise->push_error(to_async_error(cc::move(ready.value().failure.value())));
        else
            ready.value().promise->push_value(cc::move(ready.value().message));
    }
}

/// Parse whatever has arrived, answering control frames and completing a message when one is whole.
void parse_available(cc::shared_ptr<websocket_state> const& ws)
{
    for (;;)
    {
        struct step
        {
            bool have_frame = false;
            impl::ws_opcode opcode = impl::ws_opcode::text;
            cc::vector<byte> payload;
            bool is_control = false;
            bool completed_message = false;
        };

        auto outcome = ws->state.lock(
            [&](websocket_state::data& d_state) -> step
            {
                auto out = step();

                if (d_state.fatal.has_value() || d_state.closed)
                    return out;

                auto header = impl::read_frame_header(cc::span<byte const>(d_state.inbox.data(), d_state.inbox.size()));
                if (header.has_error())
                {
                    d_state.fatal = cc::move(header).error();
                    return out;
                }

                if (!header.value().has_value())
                    return out; // not a whole header yet

                auto const& frame = header.value().value();
                auto const total = frame.header_size + isize(frame.payload_length);
                if (isize(d_state.inbox.size()) < total)
                    return out; // the payload is still arriving

                // A client must mask and a server must not, and either one being wrong means the peer is speaking a
                // protocol this is not.
                if (frame.masked == ws->is_client)
                {
                    d_state.fatal = error{.code = error_code::protocol_error,
                                          .native_code = 0,
                                          .message = cc::string(ws->is_client ? "a masked frame from a server"
                                                                              : "an unmasked frame from a client")};
                    return out;
                }

                auto payload = cc::vector<byte>();
                payload.resize_to_defaulted(isize(frame.payload_length));
                for (isize i = 0; i < payload.size(); ++i)
                    payload[i] = d_state.inbox[frame.header_size + i];

                if (frame.masked)
                    impl::unmask(payload, frame.mask, 0);

                auto rest = cc::vector<byte>();
                for (auto i = total; i < d_state.inbox.size(); ++i)
                    rest.push_back(d_state.inbox[i]);
                d_state.inbox = cc::move(rest);

                out.have_frame = true;
                out.opcode = frame.opcode;
                out.is_control = impl::is_control_opcode(frame.opcode);

                if (out.is_control)
                {
                    out.payload = cc::move(payload);
                    return out;
                }

                // A data frame either starts a message or continues one, and getting that backwards is a protocol
                // error rather than something to guess at.
                if (frame.opcode == impl::ws_opcode::continuation)
                {
                    if (!d_state.partial.active)
                    {
                        d_state.fatal = error{.code = error_code::protocol_error,
                                              .native_code = 0,
                                              .message = cc::string("a websocket continuation with nothing to "
                                                                    "continue")};
                        return out;
                    }
                }
                else
                {
                    if (d_state.partial.active)
                    {
                        d_state.fatal = error{.code = error_code::protocol_error,
                                              .native_code = 0,
                                              .message = cc::string("a new websocket message before the last one "
                                                                    "finished")};
                        return out;
                    }
                    d_state.partial.active = true;
                    d_state.partial.is_text = frame.opcode == impl::ws_opcode::text;
                    d_state.partial.data.clear();
                }

                if (isize(d_state.partial.data.size()) + payload.size() > ws->max_message_bytes)
                {
                    d_state.fatal = error{.code = error_code::body_too_large,
                                          .native_code = 0,
                                          .message = cc::format("a websocket message over the {} bytes this "
                                                                "connection allows",
                                                                ws->max_message_bytes)};
                    return out;
                }

                for (auto const b : payload)
                    d_state.partial.data.push_back(b);

                if (!frame.fin)
                    return out;

                out.completed_message = true;

                auto message = websocket_message();
                message.is_text = d_state.partial.is_text;
                message.data = cc::move(d_state.partial.data);
                d_state.partial = {};

                d_state.ready.push_back(cc::move(message));
                return out;
            });

        if (!outcome.have_frame)
            break;

        // A control frame is answered here and then the loop carries on: it may sit in the middle of a fragmented
        // message, and skipping the rest of the buffer because of one would strand the message around it.
        if (outcome.is_control)
            handle_control(ws, outcome.opcode, cc::move(outcome.payload));
    }

    deliver(ws);

    // Keep reading while somebody is waiting and the connection is alive.
    auto const should_read = ws->state.lock(
        [](websocket_state::data& d_state)
        {
            return d_state.pending_receive.is_valid() && d_state.ready.empty() && !d_state.reading && !d_state.closed
                && !d_state.fatal.has_value();
        });

    if (should_read)
        pump_reads(ws);
}

void pump_reads(cc::shared_ptr<websocket_state> const& ws)
{
    auto const start = ws->state.lock(
        [](websocket_state::data& d_state)
        {
            if (d_state.reading || d_state.closed || d_state.fatal.has_value())
                return false;
            d_state.reading = true;
            return true;
        });

    if (!start)
        return;

    auto const d = ws->state.lock([](websocket_state::data const& d_state) { return d_state.receive_deadline; });

    auto buffer = cc::span<byte>(ws->read_buffer.data(), ws->read_buffer.size());
    impl::when_ready(ws->connection->receive(buffer, d, ws->token),
                     [ws](cc::shared_async<isize> const& received)
                     {
                         ws->state.lock(
                             [&](websocket_state::data& d_state)
                             {
                                 d_state.reading = false;

                                 if (received->has_error())
                                 {
                                     // Every way a connection can end looks the same here: the peer is gone, and
                                     // whoever was waiting for a message is not getting one.
                                     d_state.closed = true;
                                     return;
                                 }

                                 auto const n = received->value();
                                 for (isize i = 0; i < n; ++i)
                                     d_state.inbox.push_back(ws->read_buffer[i]);
                             });

                         parse_available(ws);
                     });
}
} // namespace

// ---- the handle ----------------------------------------------------------------------------------------

websocket::websocket(cc::shared_ptr<websocket_state> state) : _state(cc::move(state))
{
}

websocket::~websocket()
{
    close();
}

cc::shared_async<cc::unit> websocket::send_text(cc::string_view text, deadline d, cancel_token const&)
{
    auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
    return enqueue_frame(_state, impl::ws_opcode::text, bytes, d);
}

cc::shared_async<cc::unit> websocket::send_binary(cc::span<byte const> data, deadline d, cancel_token const&)
{
    return enqueue_frame(_state, impl::ws_opcode::binary, data, d);
}

cc::shared_async<websocket_message> websocket::receive(deadline d, cancel_token const&)
{
    auto promise = cc::make_async_manual<websocket_message>();

    _state->state.lock(
        [&](websocket_state::data& d_state)
        {
            CC_ASSERT(!d_state.pending_receive.is_valid(), "two receives at once on one websocket: the second would "
                                                           "take the message the first was promised");

            d_state.receive_deadline = d;
            d_state.pending_receive = promise;
        });

    // Whatever already arrived is parsed first, so a message that was waiting is handed over without another read.
    parse_available(_state);
    return promise;
}

void websocket::close(u16 code, cc::string_view reason)
{
    auto const send_close = _state->state.lock(
        [](websocket_state::data& d_state)
        {
            if (d_state.close_sent || d_state.closed)
                return false;
            d_state.close_sent = true;
            return true;
        });

    if (send_close)
    {
        auto payload = cc::vector<byte>();
        payload.push_back(byte(u8((code >> 8) & 0xFF)));
        payload.push_back(byte(u8(code & 0xFF)));
        for (auto const c : reason)
            payload.push_back(byte(c));

        (void)enqueue_frame(_state, impl::ws_opcode::close, payload, deadline::after_secs(5));
    }

    _state->state.lock([](websocket_state::data& d_state) { d_state.closed = true; });
    deliver(_state);

    if (_state->connection.is_valid())
        _state->connection->close();
}

bool websocket::is_open() const
{
    return _state->state.lock([](websocket_state::data const& d_state)
                              { return !d_state.closed && !d_state.fatal.has_value(); })
        && _state->connection.is_valid() && _state->connection->is_open();
}

cc::string_view websocket::protocol() const
{
    return _state->negotiated_protocol;
}

endpoint websocket::peer() const
{
    return _state->connection.is_valid() ? _state->connection->peer() : endpoint();
}

namespace impl
{
cc::shared_ptr<websocket> adopt_websocket(cc::shared_ptr<stream_connection> connection,
                                          bool is_client,
                                          cc::string negotiated_protocol,
                                          cc::vector<byte> leftover,
                                          isize max_message_bytes,
                                          cancel_token const& token)
{
    auto state = cc::make_shared<websocket_state>();
    state->connection = cc::move(connection);
    state->token = token;
    state->is_client = is_client;
    state->negotiated_protocol = cc::move(negotiated_protocol);
    state->max_message_bytes = max_message_bytes;
    state->read_buffer.resize_to_defaulted(k_read_chunk);

    // Bytes that arrived with the handshake belong to the stream: the peer is allowed to send its first message in
    // the same packet as its last handshake byte, and a server that drops them loses a message.
    state->state.lock([&](websocket_state::data& d_state) { d_state.inbox = cc::move(leftover); });

    return cc::make_shared<websocket>(cc::move(state));
}
} // namespace impl
} // namespace cnet
