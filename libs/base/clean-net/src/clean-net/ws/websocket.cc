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

    /// Names this frame for a cancel, which cannot hold a pointer into a vector that shifts as frames go out.
    u64 id = 0;

    /// The manual operation watching this frame's token, or null.
    /// Signalled once the frame is written or dropped, so a watch never outlives the frame it watches.
    impl::io_operation* watch = nullptr;
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
    /// Where the keepalive's timers go.
    /// A pointer rather than a reference so the state stays assignable; it never changes after adoption.
    io_system* io = nullptr;

    cc::shared_ptr<stream_connection> connection;
    cancel_token token;

    /// A client masks and a server does not; the protocol fixes which, and getting it wrong is the classic bug.
    bool is_client = true;

    cc::string negotiated_protocol;
    isize max_message_bytes = 8 * 1024 * 1024;

    i32 ping_interval_ms = 30'000;
    i32 pong_timeout_ms = 10'000;

    struct data
    {
        cc::vector<byte> inbox;
        partial_message partial;

        /// Messages that finished before anybody asked for one.
        cc::vector<websocket_message> ready;

        cc::vector<outgoing_frame> outbox;
        bool writing = false;

        /// The next frame id, so a cancel can name the queued frame it means without holding a pointer into a
        /// vector that shifts under it.
        u64 next_frame_id = 1;

        cc::shared_async<websocket_message> pending_receive;

        /// Which receive `pending_receive` is, so a deadline or a cancel armed for an earlier one does nothing.
        /// A receive is a logical operation over a shared read, so it cannot be identified by the read.
        u64 receive_generation = 0;

        /// The manual operation watching the current receive's token, or null.
        /// Signalled when the receive settles, so the watch does not outlive what it watches.
        impl::io_operation* receive_watch = nullptr;

        bool reading = false;
        bool closed = false;
        bool close_sent = false;
        cc::optional<error> fatal;

        /// A ping went out and its pong has not come back.
        bool awaiting_pong = false;

        /// Anything at all arrived since the last keepalive tick, which is what "idle" is measured against.
        bool heard_from_peer = false;

        /// The fallback mask source, for a build with no TLS backend and therefore no DRBG.
        ///
        /// A client there is a browser's, not ours, so nothing that masks for real ever reaches this.
        /// See `impl::random_mask_key`: a mask key must be UNPREDICTABLE, and a counter is not.
        u32 mask_counter = 0x9E3779B9;
    };

    cc::mutex<data> state;

    cc::vector<byte> read_buffer;
};

namespace
{
void pump_reads(cc::shared_ptr<websocket_state> const& ws);
void pump_writes(cc::shared_ptr<websocket_state> const& ws);
void arm_keepalive(cc::shared_ptr<websocket_state> const& ws, i32 delay_ms);
void deliver(cc::shared_ptr<websocket_state> const& ws);
void release_receive_watch(websocket_state::data& d_state, io_system* io);

/// End the receive numbered `generation`, if it is still the one outstanding.
///
/// **The generation is what makes this safe to arm and forget.** A deadline timer and a token watch both outlive
/// the receive they were armed for, and neither may end the receive that came after it.
void fail_receive(cc::shared_ptr<websocket_state> const& ws, u64 generation, error e)
{
    auto promise = ws->state.lock(
        [&](websocket_state::data& d_state) -> cc::shared_async<websocket_message>
        {
            if (d_state.receive_generation != generation || !d_state.pending_receive.is_valid())
                return {};

            auto taken = d_state.pending_receive;
            d_state.pending_receive = {};
            release_receive_watch(d_state, ws->io);
            return taken;
        });

    if (promise.is_valid())
        promise->push_error(to_async_error(cc::move(e)));
}

/// Signal the watch the current receive armed, so it completes rather than outliving the receive.
/// Must be called with the state locked; signalling only posts to the reactor's mailbox.
void release_receive_watch(websocket_state::data& d_state, io_system* io)
{
    if (d_state.receive_watch == nullptr)
        return;

    if (io != nullptr)
        io->signal(d_state.receive_watch);
    d_state.receive_watch = nullptr;
}

/// A manual operation that completes as `cancelled` when a caller's token is cancelled, and successfully otherwise.
///
/// **A websocket operation is logical rather than a socket operation**, so a caller's token cannot be handed to the
/// transport: one read is shared by every receive, and cancelling that read would end somebody else's.
/// This is how a token still ends the operation the caller asked about, and nothing else.
struct receive_watch final : impl::io_operation
{
    cc::unique_ptr<receive_watch> self;
    impl::cancel_registration registration;
    cc::shared_ptr<websocket_state> ws;
    u64 generation = 0;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        registration.detach();

        ws->state.lock(
            [this](websocket_state::data& d_state)
            {
                if (d_state.receive_watch == this)
                    d_state.receive_watch = nullptr;
            });

        if (!failure.has_value() || failure.value().code != error_code::cancelled)
            return;

        fail_receive(
            ws, generation,
            {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the receive was cancelled")});
    }
};
[[nodiscard]] cc::shared_async<cc::unit> enqueue_frame(cc::shared_ptr<websocket_state> const& ws,
                                                       impl::ws_opcode opcode,
                                                       cc::span<byte const> payload,
                                                       deadline d,
                                                       cancel_token const& token = {});

/// Drop the queued frame `id`, if it is still queued and not the one on the wire.
///
/// **A frame already being written cannot be cancelled**: half of it may be on the wire, and a websocket stream with
/// half a frame in it is not one either end can carry on with.
void cancel_queued_frame(cc::shared_ptr<websocket_state> const& ws, u64 id)
{
    auto promise = ws->state.lock(
        [&](websocket_state::data& d_state) -> cc::shared_async<cc::unit>
        {
            // Index 0 is the frame being written when `writing` is set, and it is the one that cannot be taken back.
            for (isize i = d_state.writing ? 1 : 0; i < d_state.outbox.size(); ++i)
            {
                if (d_state.outbox[i].id != id)
                    continue;

                auto taken = d_state.outbox[i].promise;
                for (auto j = i + 1; j < d_state.outbox.size(); ++j)
                    d_state.outbox[j - 1] = cc::move(d_state.outbox[j]);
                d_state.outbox.remove_back();
                return taken;
            }
            return {};
        });

    if (promise.is_valid())
        promise->push_error(to_async_error(
            {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the send was cancelled")}));
}

/// Watches one queued frame's token, and drops the frame if it is cancelled before it reaches the wire.
struct send_watch final : impl::io_operation
{
    cc::unique_ptr<send_watch> self;
    impl::cancel_registration registration;
    cc::shared_ptr<websocket_state> ws;
    u64 frame_id = 0;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        registration.detach();

        if (failure.has_value() && failure.value().code == error_code::cancelled)
            cancel_queued_frame(ws, frame_id);
    }
};

/// Queue a frame, and start writing if nothing else is.
[[nodiscard]] cc::shared_async<cc::unit> enqueue_frame(cc::shared_ptr<websocket_state> const& ws,
                                                       impl::ws_opcode opcode,
                                                       cc::span<byte const> payload,
                                                       deadline d,
                                                       cancel_token const& token)
{
    auto promise = cc::make_async_manual<cc::unit>();
    auto out_id = u64(0);

    auto const refused = ws->state.lock(
        [&](websocket_state::data& d_state) -> bool
        {
            if (d_state.closed || d_state.fatal.has_value())
                return true;

            auto frame = outgoing_frame();
            frame.promise = promise;
            frame.d = d;
            frame.id = d_state.next_frame_id++;
            out_id = frame.id;

            if (ws->is_client)
            {
                // A mask key must be UNPREDICTABLE, not merely different: RFC 6455 section 5.3 requires a strong
                // source of entropy, because a client that can compute its own mask can put attacker-chosen bytes on
                // the wire and poison a transparent proxy's cache -- which is the only thing masking exists to stop.
                u8 mask[4] = {};
                if (!impl::random_mask_key(mask))
                {
                    // No DRBG in this build, which is a build where the browser owns the WebSocket and nothing
                    // here is a real client.
                    // The counter keeps the framing valid; it is not doing masking's job.
                    d_state.mask_counter = d_state.mask_counter * 1664525u + 1013904223u;
                    auto const key = d_state.mask_counter;
                    mask[0] = u8(key & 0xFF);
                    mask[1] = u8((key >> 8) & 0xFF);
                    mask[2] = u8((key >> 16) & 0xFF);
                    mask[3] = u8((key >> 24) & 0xFF);
                }
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

    if (token.is_valid() && ws->io != nullptr)
    {
        auto watch = cc::make_unique<send_watch>();
        watch->kind = impl::io_op_kind::manual;
        watch->ws = ws;
        watch->frame_id = out_id;

        auto* const raw = watch.get();
        raw->self = cc::move(watch);

        ws->io->submit(raw);
        raw->registration.attach(token, *ws->io, raw);

        // The frame may already have gone out while this was being armed, in which case there is nothing left to
        // cancel and the watch is signalled rather than left pending.
        auto const still_queued = ws->state.lock(
            [&](websocket_state::data& d_state)
            {
                for (auto& frame : d_state.outbox)
                    if (frame.id == out_id)
                    {
                        frame.watch = raw;
                        return true;
                    }
                return false;
            });

        if (!still_queued)
            ws->io->signal(raw);
    }

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

                                 // This frame is on the wire, so its token has nothing left to cancel.
                                 if (d_state.outbox[0].watch != nullptr && ws->io != nullptr)
                                     ws->io->signal(d_state.outbox[0].watch);

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

/// What the next keepalive tick decided.
enum class keepalive_step : u8
{
    /// The connection is closed or already failed; the chain ends here.
    stop,

    /// Something arrived since the last tick, so there is nothing to prove.
    still_busy,

    /// Nothing arrived and nothing is outstanding: send a ping.
    send_ping,

    /// A ping went out and no pong came back.
    peer_is_gone,
};

/// One keepalive tick: ping an idle connection, and fail one whose ping went unanswered.
///
/// **The point is not the ping, it is the pong that does not arrive.**
/// A peer whose machine vanished sends no FIN, so a receive on that connection waits exactly as long as one on a
/// quiet connection would -- which, with the default deadline, is forever.
void keepalive_tick(cc::shared_ptr<websocket_state> const& ws)
{
    auto const step = ws->state.lock(
        [](websocket_state::data& d_state)
        {
            if (d_state.closed || d_state.fatal.has_value())
                return keepalive_step::stop;

            if (d_state.awaiting_pong)
                return keepalive_step::peer_is_gone;

            if (d_state.heard_from_peer)
            {
                d_state.heard_from_peer = false;
                return keepalive_step::still_busy;
            }

            d_state.awaiting_pong = true;
            return keepalive_step::send_ping;
        });

    switch (step)
    {
    case keepalive_step::stop:
        return;

    case keepalive_step::still_busy:
        arm_keepalive(ws, ws->ping_interval_ms);
        return;

    case keepalive_step::send_ping:
        // An empty ping: the payload is echoed by the peer and nothing here reads it, so carrying one would only be
        // bytes to check.
        (void)enqueue_frame(ws, impl::ws_opcode::ping, {}, deadline::after_ms(ws->pong_timeout_ms));

        // And start reading if nothing is: a keepalive that only works while the caller happens to be receiving is
        // one that fails exactly when it is needed.
        pump_reads(ws);

        // The next tick is the deadline for the pong rather than the next ping, which is what makes one timer do
        // both jobs.
        arm_keepalive(ws, ws->pong_timeout_ms);
        return;

    case keepalive_step::peer_is_gone:
        ws->state.lock(
            [](websocket_state::data& d_state)
            {
                d_state.fatal = error{.code = error_code::timed_out,
                                      .native_code = 0,
                                      .message = cc::string("the peer did not answer a keepalive ping")};
                d_state.closed = true;
            });

        if (ws->connection.is_valid())
            ws->connection->close();

        deliver(ws);
        return;
    }
}

void arm_keepalive(cc::shared_ptr<websocket_state> const& ws, i32 delay_ms)
{
    if (ws->io == nullptr || delay_ms <= 0)
        return;

    // The timer holds the state alive, so a WebSocket nobody references any more is freed one tick late rather than
    // at once -- its connection is closed immediately either way, which is the part that holds a resource.
    impl::run_after(*ws->io, delay_ms, [ws] { keepalive_tick(ws); });
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
    {
        // Which ping it answers is not checked: the payload is echoed and any pong at all proves the peer is there,
        // which is the only thing the keepalive is asking.
        ws->state.lock([](websocket_state::data& d_state) { d_state.awaiting_pong = false; });
        return;
    }

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
        [&ws](websocket_state::data& d_state) -> cc::optional<delivery>
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
                release_receive_watch(d_state, ws->io);
                return out;
            }

            if (d_state.fatal.has_value())
            {
                auto out = delivery{.promise = d_state.pending_receive, .failure = d_state.fatal};
                d_state.pending_receive = {};
                release_receive_watch(d_state, ws->io);
                return out;
            }

            if (d_state.closed)
            {
                auto out = delivery{.promise = d_state.pending_receive,
                                    .failure = error{.code = error_code::connection_closed,
                                                     .native_code = 0,
                                                     .message = cc::string("the peer closed the websocket")}};
                d_state.pending_receive = {};
                release_receive_watch(d_state, ws->io);
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

    auto const should_read = ws->state.lock(
        [](websocket_state::data& d_state)
        {
            if (d_state.reading || d_state.closed || d_state.fatal.has_value())
                return false;

            // Somebody is waiting for a message and nothing is queued for them.
            if (d_state.pending_receive.is_valid() && d_state.ready.empty())
                return true;

            // Or a ping is outstanding, and the pong arrives nowhere unless somebody reads.
            // `ready` staying empty is the bound, so a peer cannot use the pong window to make us buffer.
            return d_state.awaiting_pong && d_state.ready.empty();
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

    // The read carries the KEEPALIVE's bound and nothing else.
    // A caller's deadline belongs to its receive, which has a timer of its own: one read is shared by every receive,
    // so a deadline handed to the read outlives the caller who asked for it and ends a connection nobody said was
    // dead.
    // With keepalives off there is nothing left to bound it, which is what `never()` says.
    auto const d = ws->ping_interval_ms > 0 ? deadline::after_ms(i64(ws->ping_interval_ms) + i64(ws->pong_timeout_ms))
                                            : deadline::never();

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
                                     // The connection is over either way, but WHY is not the same fact: a peer that
                                     // hung up is the ordinary end of a websocket, and one that stopped answering
                                     // a ping is a failure.
                                     // Collapsing them reports the wrong one.
                                     //
                                     // The read's own code cannot be read back -- `cc::any_error` erases a
                                     // `cnet::error` to its message -- so the distinction is drawn from what this
                                     // side already knows: a read that ended while a ping was outstanding ended
                                     // because the peer never answered it.
                                     d_state.closed = true;
                                     if (d_state.awaiting_pong && !d_state.fatal.has_value())
                                         d_state.fatal = error{.code = error_code::timed_out,
                                                               .native_code = 0,
                                                               .message = cc::string("the peer did not answer a "
                                                                                     "keepalive ping")};
                                     return;
                                 }

                                 auto const n = received->value();
                                 if (n > 0)
                                     d_state.heard_from_peer = true;

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

cc::shared_async<cc::unit> websocket::send_text(cc::string_view text, deadline d, cancel_token const& token)
{
    auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
    return enqueue_frame(_state, impl::ws_opcode::text, bytes, d, token);
}

cc::shared_async<cc::unit> websocket::send_binary(cc::span<byte const> data, deadline d, cancel_token const& token)
{
    return enqueue_frame(_state, impl::ws_opcode::binary, data, d, token);
}

cc::shared_async<websocket_message> websocket::receive(deadline d, cancel_token const& token)
{
    auto promise = cc::make_async_manual<websocket_message>();

    auto const generation = _state->state.lock(
        [&](websocket_state::data& d_state)
        {
            CC_ASSERT(!d_state.pending_receive.is_valid(), "two receives at once on one websocket: the second would "
                                                           "take the message the first was promised");

            d_state.pending_receive = promise;
            return ++d_state.receive_generation;
        });

    // The deadline is the RECEIVE's, so it gets a timer of its own rather than being handed to the shared read.
    if (d.is_finite() && _state->io != nullptr)
        impl::run_after(*_state->io, d.timeout_ms,
                        [ws = _state, generation]
                        {
                            fail_receive(ws, generation,
                                         {.code = error_code::timed_out,
                                          .native_code = 0,
                                          .message = cc::string("no websocket message arrived in time")});
                        });

    // And the token ends this receive rather than the connection: the read underneath is shared, and cancelling it
    // would end somebody else's receive as well as this one.
    if (token.is_valid() && _state->io != nullptr)
    {
        auto watch = cc::make_unique<receive_watch>();
        watch->kind = impl::io_op_kind::manual;
        watch->ws = _state;
        watch->generation = generation;

        auto* const raw = watch.get();
        raw->self = cc::move(watch);

        _state->state.lock([&](websocket_state::data& d_state) { d_state.receive_watch = raw; });

        _state->io->submit(raw);
        raw->registration.attach(token, *_state->io, raw);
    }

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
cc::shared_ptr<websocket> adopt_websocket(io_system& io, websocket_adoption adoption)
{
    auto state = cc::make_shared<websocket_state>();
    state->io = &io;
    state->connection = cc::move(adoption.connection);
    state->token = adoption.token;
    state->is_client = adoption.is_client;
    state->negotiated_protocol = cc::move(adoption.negotiated_protocol);
    state->max_message_bytes = adoption.max_message_bytes;
    state->ping_interval_ms = adoption.ping_interval_ms;
    state->pong_timeout_ms = adoption.pong_timeout_ms;
    state->read_buffer.resize_to_defaulted(k_read_chunk);

    // Bytes that arrived with the handshake belong to the stream: the peer is allowed to send its first message in
    // the same packet as its last handshake byte, and a server that drops them loses a message.
    state->state.lock([&](websocket_state::data& d_state) { d_state.inbox = cc::move(adoption.leftover); });

    auto socket = cc::make_shared<websocket>(state);
    arm_keepalive(state, state->ping_interval_ms);
    return socket;
}
} // namespace impl
} // namespace cnet
