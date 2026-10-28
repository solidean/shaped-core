#include "virtual_transport.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/reactor.hh>
#include <clean-net/transport/datagram.hh>

#include <memory> // std::unique_ptr, to own a backend through its interface -- see .shaped-lint.yml

// A virtual network is two things: a registry of who is listening where, and a byte queue per direction.
//
// WHAT MAKES IT SAFE WITHOUT BEING SLOW.
// Every queue is behind a cc::mutex, so a send from any thread and a receive completing on the reactor thread cannot
// race.
// The reactor is used for exactly one thing here -- PARKING: a receive with nothing to read, and an accept with
// nobody knocking, become `manual` operations that the writing side signals.
// That is what gives them the io_system's deadline for free, rather than a second timeout mechanism that would drift
// from the real one.
//
// A send never parks: the queue is unbounded and the send completes at once.
// Backpressure and bandwidth belong to the simulating transport, not here -- this one is meant to be the fastest and
// most predictable thing a test can run against.

namespace cnet
{
namespace
{
/// One direction of a connection: what one end writes and the other reads.
struct pipe_data
{
    cc::vector<byte> data;

    /// How far the reader has consumed; the buffer is cleared rather than shifted once it catches up.
    isize read_pos = 0;

    /// The writer will send nothing more, which is what turns an empty queue into end-of-stream.
    bool writer_done = false;

    /// The one receive waiting on this pipe, signalled by whoever writes next.
    /// Two concurrent receives on one connection are a caller error, so there is never more than one.
    impl::io_operation* parked = nullptr;

    [[nodiscard]] isize available() const { return data.size() - read_pos; }
};

using pipe = cc::mutex<pipe_data>;

/// Hand whatever is queued to a waiting reader, and say who to wake afterwards.
/// The signal happens OUTSIDE the lock, because a completion handler takes the same lock to read its bytes.
[[nodiscard]] impl::io_operation* take_parked(pipe& p)
{
    return p.lock(
        [](pipe_data& d)
        {
            auto* const op = d.parked;
            d.parked = nullptr;
            return op;
        });
}

/// One datagram on its way, held by the socket it was sent to.
struct virtual_datagram
{
    endpoint from;
    cc::vector<byte> bytes;

    /// Delivery is instant, so the moment it was sent is the moment it arrived.
    i64 arrived_at_ns = 0;
};

struct virtual_datagram_inbox
{
    cc::vector<virtual_datagram> queue;

    /// How far the reader has consumed; the queue is cleared rather than shifted once it catches up.
    isize head = 0;

    /// The one readiness wait on this socket, signalled by whoever delivers next.
    impl::io_operation* parked = nullptr;

    [[nodiscard]] bool has_any() const { return head < queue.size(); }
};

/// What a datagram socket and the network that routes to it both point at.
struct virtual_datagram_endpoint
{
    endpoint where;
    cc::mutex<virtual_datagram_inbox> inbox;
};

/// What a listener and the network that made it both point at.
struct virtual_listener_state
{
    endpoint where;
    cc::mutex<cc::vector<cc::shared_ptr<stream_connection>>> incoming;
    cc::mutex<impl::io_operation*> parked_accept;
};

/// A receive that had nothing to read, waiting for bytes or for its deadline.
struct virtual_receive_op final : impl::io_operation
{
    cc::shared_async<isize> promise;
    cc::unique_ptr<virtual_receive_op> self;
    impl::cancel_registration cancellation;
    cc::shared_ptr<pipe> inbox;
    byte* out = nullptr;
    isize out_size = 0;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        // Deregister first: a deadline can fire while the pipe still points here, and the next writer would then
        // signal an operation that has already gone.
        inbox->lock(
            [this](pipe_data& d)
            {
                if (d.parked == this)
                    d.parked = nullptr;
            });

        if (failure.has_value())
        {
            promise->push_error(to_async_error(cc::move(failure.value())));
            return;
        }

        auto const taken = inbox->lock(
            [this](pipe_data& d) -> isize
            {
                auto const n = d.available() < out_size ? d.available() : out_size;
                for (isize i = 0; i < n; ++i)
                    out[i] = d.data[d.read_pos + i];
                d.read_pos += n;
                if (d.read_pos == d.data.size())
                {
                    d.data.clear();
                    d.read_pos = 0;
                }
                return n;
            });

        if (taken > 0)
        {
            promise->push_value(taken);
            return;
        }

        // Signalled with nothing to take means the writer finished, which is the only other reason to wake a reader.
        promise->push_error(to_async_error({.code = error_code::connection_closed,
                                            .native_code = 0,
                                            .message = cc::string("the peer closed the connection")}));
    }
};

/// An accept with nobody knocking yet.
struct virtual_accept_op final : impl::io_operation
{
    cc::shared_async<cc::shared_ptr<stream_connection>> promise;
    cc::unique_ptr<virtual_accept_op> self;
    impl::cancel_registration cancellation;
    cc::shared_ptr<virtual_listener_state> listener;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        listener->parked_accept.lock(
            [this](impl::io_operation*& slot)
            {
                if (slot == this)
                    slot = nullptr;
            });

        if (failure.has_value())
        {
            promise->push_error(to_async_error(cc::move(failure.value())));
            return;
        }

        auto taken = listener->incoming.lock(
            [](cc::vector<cc::shared_ptr<stream_connection>>& q) -> cc::shared_ptr<stream_connection>
            {
                if (q.empty())
                    return {};
                auto front = cc::move(q[0]);
                for (isize i = 1; i < q.size(); ++i)
                    q[i - 1] = cc::move(q[i]);
                q.remove_back();
                return front;
            });

        if (taken.is_valid())
        {
            promise->push_value(cc::move(taken));
            return;
        }

        promise->push_error(to_async_error({.code = error_code::cancelled,
                                            .native_code = 0,
                                            .message = cc::string("the listener was woken with nothing to accept")}));
    }
};

/// One end of a virtual connection: it reads from `_inbox` and writes into `_outbox`.
class virtual_connection final : public connection_backend
{
public:
    virtual_connection(io_system& io,
                       cc::shared_ptr<pipe> inbox,
                       cc::shared_ptr<pipe> outbox,
                       endpoint local_endpoint,
                       endpoint peer_endpoint)
      : _io(io), _inbox(cc::move(inbox)), _outbox(cc::move(outbox)), _local(local_endpoint), _peer(peer_endpoint)
    {
    }

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token) override
    {
        if (!_open)
            return impl::failed_async<isize>({.code = error_code::connection_closed,
                                              .native_code = 0,
                                              .message = cc::string("the connection is closed")});

        // An already-cancelled token fails here rather than starting work nobody wants.
        if (token.is_cancelled())
            return impl::failed_async<isize>(
                {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});

        // The fast path: bytes are already here, so nothing reaches the reactor at all.
        auto promise = cc::make_async_manual<isize>();
        auto const taken = _inbox->lock(
            [&](pipe_data& d) -> isize
            {
                auto const n = d.available() < buffer.size() ? d.available() : buffer.size();
                for (isize i = 0; i < n; ++i)
                    buffer[i] = d.data[d.read_pos + i];
                d.read_pos += n;
                if (d.read_pos == d.data.size())
                {
                    d.data.clear();
                    d.read_pos = 0;
                }
                return n;
            });

        if (taken > 0)
        {
            promise->push_value(taken);
            return promise;
        }

        if (_inbox->lock([](pipe_data const& d) { return d.writer_done; }))
            return impl::failed_async<isize>({.code = error_code::connection_closed,
                                              .native_code = 0,
                                              .message = cc::string("the peer closed the connection")});

        // Nothing to read: park on the reactor, where the deadline is the io_system's own.
        auto op = cc::make_unique<virtual_receive_op>();
        op->kind = impl::io_op_kind::manual;
        op->deadline_ns = deadline_to_absolute(_io, d);
        op->promise = promise;
        op->inbox = _inbox;
        op->out = buffer.data();
        op->out_size = buffer.size();

        auto* const raw = op.get();
        raw->self = cc::move(op);

        // SUBMIT FIRST, PARK SECOND, THEN LOOK AGAIN -- and each of those three is one race.
        //
        // Publishing to the pipe before submitting would let a writer signal an operation the reactor has never seen,
        // and such a signal is dropped: the submit goes first, and the actor's mailbox keeps the two in order.
        // Parking after that closes the other side, where a writer between the empty check above and here finds no
        // reader to signal and the bytes sit in the pipe with nobody coming for them.
        // Looking again is what covers the gap that is left: if something arrived while we were submitting, nothing
        // parked in time to be signalled, so we wake ourselves.
        auto in_flight = _io.submit(raw);

        // An empty guard means the io_system was stopping, and `submit` already answered the operation and freed it.
        if (in_flight.io() == nullptr)
            return promise;

        auto const arrived_meanwhile = _inbox->lock(
            [raw](pipe_data& d)
            {
                if (d.available() > 0 || d.writer_done)
                    return true;

                d.parked = raw;
                return false;
            });

        if (arrived_meanwhile)
            _io.signal(raw);

        // Last, so the guard outlives the park above: `d.parked = raw` puts the operation in shared state, and a
        // completion between the two would leave a freed pointer there.
        raw->cancellation.attach(cc::move(in_flight), token);

        return promise;
    }

    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline, cancel_token const& token) override
    {
        if (!_open)
            return impl::failed_async<cc::unit>({.code = error_code::connection_closed,
                                                 .native_code = 0,
                                                 .message = cc::string("the connection is closed")});

        // A send never parks here, so this is the only place a cancelled token can stop one.
        if (token.is_cancelled())
            return impl::failed_async<cc::unit>(
                {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});

        if (_outbox->lock([](pipe_data const& d) { return d.writer_done; }))
            return impl::failed_async<cc::unit>({.code = error_code::connection_closed,
                                                 .native_code = 0,
                                                 .message = cc::string("this half of the connection is shut down")});

        _outbox->lock(
            [&](pipe_data& d)
            {
                for (auto b : bytes)
                    d.data.push_back(b);
            });

        auto* const waiting = take_parked(*_outbox);
        if (waiting != nullptr)
            _io.signal(waiting);

        auto promise = cc::make_async_manual<cc::unit>();
        promise->push_value(cc::unit{});
        return promise;
    }

    cc::result<cc::unit, error> shutdown_send() override
    {
        if (!_open)
            return cc::error(error{.code = error_code::connection_closed,
                                   .native_code = 0,
                                   .message = cc::string("the connection is closed")});
        finish_writing();
        return cc::unit{};
    }

    [[nodiscard]] endpoint local() const override { return _local; }
    [[nodiscard]] endpoint peer() const override { return _peer; }
    [[nodiscard]] bool is_open() const override { return _open; }

    void close() override
    {
        if (!_open)
            return;
        _open = false;
        finish_writing();
    }

    ~virtual_connection() override { close(); }

private:
    /// Tell the far end that nothing more is coming, and wake it if it is waiting to hear that.
    void finish_writing()
    {
        _outbox->lock([](pipe_data& d) { d.writer_done = true; });

        auto* const waiting = take_parked(*_outbox);
        if (waiting != nullptr)
            _io.signal(waiting);
    }

    io_system& _io;
    cc::shared_ptr<pipe> _inbox;
    cc::shared_ptr<pipe> _outbox;
    endpoint _local;
    endpoint _peer;
    bool _open = true;
};

} // namespace

/// The registry, shared by the network and every listener it hands out.
struct virtual_network::state
{
    io_system& io;

    /// A short list rather than a map: a test has a handful of listeners, and `endpoint` has no hash of its own.
    cc::mutex<cc::vector<cc::shared_ptr<virtual_listener_state>>> listeners;

    /// Every bound datagram socket, a short list for the same reason.
    cc::mutex<cc::vector<cc::shared_ptr<virtual_datagram_endpoint>>> datagram_endpoints;

    /// Where a synthetic port comes from, for a bind to 0 and for the far end of every connect.
    cc::atomic<i32> next_port = 40000;

    explicit state(io_system& s) : io(s) {}
};

namespace
{
/// A listener on a virtual network.
class virtual_listener final : public listener_backend
{
public:
    virtual_listener(cc::shared_ptr<virtual_network::state> net, cc::shared_ptr<virtual_listener_state> s)
      : _net(cc::move(net)), _state(cc::move(s))
    {
    }

    [[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> accept(deadline d, cancel_token const& token) override
    {
        if (token.is_cancelled())
            return impl::failed_async<cc::shared_ptr<stream_connection>>(
                {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});

        auto promise = cc::make_async_manual<cc::shared_ptr<stream_connection>>();

        auto taken = _state->incoming.lock(
            [](cc::vector<cc::shared_ptr<stream_connection>>& q) -> cc::shared_ptr<stream_connection>
            {
                if (q.empty())
                    return {};
                auto front = cc::move(q[0]);
                for (isize i = 1; i < q.size(); ++i)
                    q[i - 1] = cc::move(q[i]);
                q.remove_back();
                return front;
            });

        if (taken.is_valid())
        {
            promise->push_value(cc::move(taken));
            return promise;
        }

        auto op = cc::make_unique<virtual_accept_op>();
        op->kind = impl::io_op_kind::manual;
        op->deadline_ns = deadline_to_absolute(_net->io, d);
        op->promise = promise;
        op->listener = _state;

        auto* const raw = op.get();
        raw->self = cc::move(op);

        // Submitted, then parked, for the same reasons a receive is.
        //
        // A receive parks under the pipe's own lock, so its check and its park are one step.
        // An accept has two mutexes and cannot, so `incoming` is read INSIDE `parked_accept` -- and the connect side
        // takes them one after the other rather than nested, which is what makes that order safe.
        // Reading them the other way round would leave the window this is here to close.
        auto in_flight = _net->io.submit(raw);

        // An empty guard means the io_system was stopping, and `submit` already answered the operation and freed it.
        if (in_flight.io() == nullptr)
            return promise;

        auto const arrived_meanwhile = _state->parked_accept.lock(
            [raw, this](impl::io_operation*& slot)
            {
                if (!_state->incoming.lock([](cc::vector<cc::shared_ptr<stream_connection>> const& q)
                                           { return q.empty(); }))
                    return true;

                slot = raw;
                return false;
            });

        if (arrived_meanwhile)
            _net->io.signal(raw);

        // Last, for the same reason the receive attaches last: `slot = raw` is the park, and the guard has to outlive
        // it.
        raw->cancellation.attach(cc::move(in_flight), token);

        return promise;
    }

    [[nodiscard]] endpoint local() const override { return _state->where; }

    ~virtual_listener() override
    {
        // Leaving the registry is what makes a later connect report `connection_refused` rather than queueing into
        // a listener nobody will ever accept from.
        _net->listeners.lock(
            [this](cc::vector<cc::shared_ptr<virtual_listener_state>>& all)
            {
                for (isize i = 0; i < all.size(); ++i)
                    if (all[i].get() == _state.get())
                    {
                        all[i] = cc::move(all[all.size() - 1]);
                        all.remove_back();
                        return;
                    }
            });
    }

private:
    cc::shared_ptr<virtual_network::state> _net;
    cc::shared_ptr<virtual_listener_state> _state;
};
} // namespace

namespace
{
/// Whether a socket bound at `a` keeps another from binding `b`, the way a real stack without address reuse decides:
/// the same port and family, and the same address or either one bound to every interface.
[[nodiscard]] bool datagram_binds_clash(endpoint const& a, endpoint const& b)
{
    if (a.port != b.port || a.address.family() != b.address.family())
        return false;
    return a.address == b.address || a.address.is_unspecified() || b.address.is_unspecified();
}

/// The largest payload one IPv4 datagram carries, which a virtual network enforces so a test does not pass here
/// with a datagram no real socket would send.
constexpr isize k_max_datagram_bytes = 65507;

/// A readiness wait on a virtual datagram socket, parked until something is delivered.
struct virtual_readable_op final : impl::io_operation
{
    cc::shared_async<cc::unit> promise;
    cc::unique_ptr<virtual_readable_op> self;
    impl::cancel_registration cancellation;
    cc::shared_ptr<virtual_datagram_endpoint> owner;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        owner->inbox.lock(
            [this](virtual_datagram_inbox& d)
            {
                if (d.parked == this)
                    d.parked = nullptr;
            });

        if (failure.has_value())
            promise->push_error(to_async_error(cc::move(failure.value())));
        else
            promise->push_value(cc::unit{});
    }
};

/// A datagram socket on a virtual network.
///
/// A send finds the receiving socket in the network's registry and appends a copy to its inbox; nobody bound there
/// means the datagram is gone, exactly as on a real network.
class virtual_datagram_backend final : public datagram_backend
{
public:
    virtual_datagram_backend(cc::shared_ptr<virtual_network::state> net, cc::shared_ptr<virtual_datagram_endpoint> self)
      : _net(cc::move(net)), _self(cc::move(self))
    {
    }

    [[nodiscard]] cc::result<isize, error> receive_batch(cc::span<datagram> records) override
    {
        return _self->inbox.lock(
            [&](virtual_datagram_inbox& d) -> isize
            {
                auto filled = isize(0);
                while (filled < records.size() && d.has_any())
                {
                    auto const& from = d.queue[d.head];
                    auto& r = records[filled];
                    auto const n = cc::min(from.bytes.size(), r.buffer.size());
                    if (n > 0)
                        cc::memcpy(r.buffer.data(), from.bytes.data(), n);
                    r.size = n;
                    r.peer = from.from;
                    r.received_at_ns = from.arrived_at_ns;
                    r.truncated = n < from.bytes.size();
                    ++d.head;
                    ++filled;
                }
                if (!d.has_any())
                {
                    d.queue.clear();
                    d.head = 0;
                }
                return filled;
            });
    }

    [[nodiscard]] cc::result<isize, error> send_batch(cc::span<outgoing_datagram const> packets) override
    {
        auto const now = _net->io.time_source().now_ns();
        for (isize i = 0; i < packets.size(); ++i)
        {
            auto const& p = packets[i];
            auto refused = cc::optional<error>();
            if (!p.peer.address.is_valid() || p.peer.address.is_unspecified())
                refused = error{.code = error_code::invalid_argument,
                                .native_code = 0,
                                .message = cc::format("send: {} is not a destination", p.peer)};
            else if (p.bytes.size() > k_max_datagram_bytes)
                refused = error{.code = error_code::invalid_argument,
                                .native_code = 0,
                                .message = cc::format("send: a datagram of {} bytes is larger than any network carries",
                                                      p.bytes.size())};

            // The partial-batch rule: an error only when nothing went before it.
            if (refused.has_value())
            {
                if (i > 0)
                    return i;
                return cc::error(cc::move(refused.value()));
            }

            deliver(p, now);
        }
        return packets.size();
    }

    [[nodiscard]] cc::shared_async<cc::unit> wait_readable(cancel_token const& token) override
    {
        auto op = cc::make_unique<virtual_readable_op>();
        op->kind = impl::io_op_kind::manual;
        op->owner = _self;

        auto promise = cc::make_async_manual<cc::unit>();
        op->promise = promise;

        auto* const raw = op.get();
        raw->self = cc::move(op);

        // Submit, park, look again -- the order a virtual receive uses, for the same races.
        // Something already waiting signals at once rather than completing here: a wait never completes inline.
        auto in_flight = _net->io.submit(raw);

        // An empty guard means the io_system was stopping, and `submit` already answered the operation and freed it.
        if (in_flight.io() == nullptr)
            return promise;

        auto const ready = _self->inbox.lock(
            [raw](virtual_datagram_inbox& d)
            {
                if (d.has_any())
                    return true;
                d.parked = raw;
                return false;
            });

        if (ready)
            _net->io.signal(raw);

        raw->cancellation.attach(cc::move(in_flight), token);
        return promise;
    }

    [[nodiscard]] endpoint local() const override { return _self->where; }
    [[nodiscard]] io_system& io() const override { return _net->io; }

    ~virtual_datagram_backend() override
    {
        // Leaving the registry is what makes later datagrams to this endpoint vanish rather than pile up.
        _net->datagram_endpoints.lock(
            [this](cc::vector<cc::shared_ptr<virtual_datagram_endpoint>>& all)
            {
                for (isize i = 0; i < all.size(); ++i)
                    if (all[i].get() == _self.get())
                    {
                        all[i] = cc::move(all[all.size() - 1]);
                        all.remove_back();
                        return;
                    }
            });
    }

private:
    /// Hand one datagram to whoever is bound at its destination, if anyone is.
    void deliver(outgoing_datagram const& p, i64 now)
    {
        // An exact match first, then a socket bound to every interface on that port, the way a real stack routes.
        auto target = _net->datagram_endpoints.lock(
            [&](cc::vector<cc::shared_ptr<virtual_datagram_endpoint>> const& all) -> cc::shared_ptr<virtual_datagram_endpoint>
            {
                auto wildcard = cc::shared_ptr<virtual_datagram_endpoint>();
                for (auto const& e : all)
                {
                    if (e->where == p.peer)
                        return e;
                    if (e->where.port == p.peer.port && e->where.address.is_unspecified()
                        && e->where.address.family() == p.peer.address.family())
                        wildcard = e;
                }
                return wildcard;
            });

        if (!target.is_valid())
            return;

        // A socket bound to every interface has no single address to be seen from, so the receiver sees the one the
        // datagram was sent to, as it would on one machine.
        auto from = _self->where;
        if (from.address.is_unspecified())
            from.address = p.peer.address;

        auto copy = virtual_datagram{.from = from, .bytes = {}, .arrived_at_ns = now};
        copy.bytes.resize_to_defaulted(p.bytes.size());
        if (p.bytes.size() > 0)
            cc::memcpy(copy.bytes.data(), p.bytes.data(), p.bytes.size());

        auto* const waiting = target->inbox.lock(
            [&](virtual_datagram_inbox& d)
            {
                d.queue.push_back(cc::move(copy));
                auto* const op = d.parked;
                d.parked = nullptr;
                return op;
            });

        // Outside the lock, because the completion takes the same lock to deregister.
        if (waiting != nullptr)
            _net->io.signal(waiting);
    }

    cc::shared_ptr<virtual_network::state> _net;
    cc::shared_ptr<virtual_datagram_endpoint> _self;
};
} // namespace

// ---- the network ---------------------------------------------------------------------------------------

virtual_network::virtual_network(io_system& io) : _state(cc::make_shared<state>(io))
{
}

virtual_network::~virtual_network() = default;

cc::result<cc::unique_ptr<stream_listener>, error> virtual_network::listen(endpoint const& where,
                                                                           tcp_listen_options const& /*options*/)
{
    if (!where.address.is_valid())
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("listen: the endpoint has no address")});

    auto bound = where;
    if (bound.port == 0)
        bound.port = _state->next_port.fetch_add(1);

    auto const taken = _state->listeners.lock(
        [&](cc::vector<cc::shared_ptr<virtual_listener_state>> const& all)
        {
            for (auto const& l : all)
                if (l->where == bound)
                    return true;
            return false;
        });

    if (taken)
        return cc::error(error{.code = error_code::address_in_use,
                               .native_code = 0,
                               .message = cc::format("{} already has a virtual listener", bound)});

    auto listener_state = cc::make_shared<virtual_listener_state>();
    listener_state->where = bound;
    _state->listeners.lock([&](cc::vector<cc::shared_ptr<virtual_listener_state>>& all)
                           { all.push_back(listener_state); });

    CC_LOG_TRACE("virtual listener on {}", bound);
    return cc::make_unique<stream_listener>(std::make_unique<virtual_listener>(_state, cc::move(listener_state)));
}

io_system& virtual_network::io() const
{
    return _state->io;
}

cc::shared_async<cc::shared_ptr<stream_connection>> virtual_network::connect(endpoint const& where,
                                                                             deadline /*d*/,
                                                                             tcp_options const& /*options*/,
                                                                             cancel_token const& token)
{
    using handle = cc::shared_ptr<stream_connection>;

    // A virtual connect never waits, so a token can only stop it before it starts.
    if (token.is_cancelled())
        return impl::failed_async<handle>(
            {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});

    auto server = _state->listeners.lock(
        [&](cc::vector<cc::shared_ptr<virtual_listener_state>> const& all) -> cc::shared_ptr<virtual_listener_state>
        {
            for (auto const& l : all)
                if (l->where == where)
                    return l;
            return {};
        });

    // Nobody listening is a refusal rather than a wait, exactly as a real stack answers a closed port.
    if (!server.is_valid())
        return impl::failed_async<handle>({.code = error_code::connection_refused,
                                           .native_code = 0,
                                           .message = cc::format("nothing is listening on {}", where)});

    auto const client_endpoint = endpoint(where.address, _state->next_port.fetch_add(1));

    auto to_server = cc::make_shared<pipe>();
    auto to_client = cc::make_shared<pipe>();

    auto client = cc::make_shared<stream_connection>(
        std::make_unique<virtual_connection>(_state->io, to_client, to_server, client_endpoint, where));
    auto accepted = cc::make_shared<stream_connection>(
        std::make_unique<virtual_connection>(_state->io, to_server, to_client, where, client_endpoint));

    server->incoming.lock([&](cc::vector<handle>& q) { q.push_back(cc::move(accepted)); });

    auto* const waiting = server->parked_accept.lock(
        [](impl::io_operation*& slot)
        {
            auto* const op = slot;
            slot = nullptr;
            return op;
        });
    if (waiting != nullptr)
        _state->io.signal(waiting);

    auto promise = cc::make_async_manual<handle>();
    promise->push_value(cc::move(client));
    return promise;
}

cc::result<cc::unique_ptr<datagram_socket>, error> virtual_network::bind_datagram(endpoint const& where,
                                                                                  datagram_options const& /*options*/)
{
    if (!where.address.is_valid())
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("bind: the endpoint has no address")});

    auto self = cc::make_shared<virtual_datagram_endpoint>();
    auto bound = where;

    // Checked and registered in one step, so two binds racing for one port cannot both win.
    // A port of 0 takes the next free one, skipping any a caller bound explicitly.
    auto const taken = _state->datagram_endpoints.lock(
        [&](cc::vector<cc::shared_ptr<virtual_datagram_endpoint>>& all)
        {
            auto const picking = bound.port == 0;
            while (true)
            {
                if (picking)
                    bound.port = _state->next_port.fetch_add(1);

                auto clash = false;
                for (auto const& e : all)
                    if (datagram_binds_clash(e->where, bound))
                        clash = true;

                if (!clash)
                    break;
                if (!picking)
                    return true;
            }

            self->where = bound;
            all.push_back(self);
            return false;
        });

    if (taken)
        return cc::error(error{.code = error_code::address_in_use,
                               .native_code = 0,
                               .message = cc::format("{} already has a virtual datagram socket", bound)});

    CC_LOG_TRACE("virtual datagram socket on {}", bound);
    return cc::make_unique<datagram_socket>(std::make_unique<virtual_datagram_backend>(_state, cc::move(self)));
}
} // namespace cnet
