#include "virtual_transport.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/reactor.hh>

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

/// What a listener and the network that made it both point at.
struct virtual_listener_state
{
    endpoint where;
    cc::mutex<cc::vector<cc::shared_ptr<tcp_connection>>> incoming;
    cc::mutex<impl::io_operation*> parked_accept;
};

/// A receive that had nothing to read, waiting for bytes or for its deadline.
struct virtual_receive_op final : impl::io_operation
{
    cc::shared_async<isize> promise;
    cc::unique_ptr<virtual_receive_op> self;
    cc::shared_ptr<pipe> inbox;
    byte* out = nullptr;
    isize out_size = 0;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);

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
            promise->push_error(cc::async_error::make_error(cc::any_error(cc::move(failure.value()))));
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
        promise->push_error(cc::async_error::make_error(cc::any_error(error{.code = error_code::connection_closed,
                                                                            .native_code = 0,
                                                                            .message = cc::string("the peer closed the "
                                                                                                  "connection")})));
    }
};

/// An accept with nobody knocking yet.
struct virtual_accept_op final : impl::io_operation
{
    cc::shared_async<cc::shared_ptr<tcp_connection>> promise;
    cc::unique_ptr<virtual_accept_op> self;
    cc::shared_ptr<virtual_listener_state> listener;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);

        listener->parked_accept.lock(
            [this](impl::io_operation*& slot)
            {
                if (slot == this)
                    slot = nullptr;
            });

        if (failure.has_value())
        {
            promise->push_error(cc::async_error::make_error(cc::any_error(cc::move(failure.value()))));
            return;
        }

        auto taken = listener->incoming.lock(
            [](cc::vector<cc::shared_ptr<tcp_connection>>& q) -> cc::shared_ptr<tcp_connection>
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

        promise->push_error(cc::async_error::make_error(
            cc::any_error(error{.code = error_code::cancelled,
                                .native_code = 0,
                                .message = cc::string("the listener was woken with nothing to accept")})));
    }
};

/// A promise that is already settled, for the common case where nothing has to wait at all.
template <class T>
[[nodiscard]] cc::shared_async<T> failed_async(error e)
{
    auto promise = cc::make_async_manual<T>();
    promise->push_error(cc::async_error::make_error(cc::any_error(cc::move(e))));
    return promise;
}

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

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d) override
    {
        if (!_open)
            return failed_async<isize>({.code = error_code::connection_closed,
                                        .native_code = 0,
                                        .message = cc::string("the connection is closed")});

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
            return failed_async<isize>({.code = error_code::connection_closed,
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
        _inbox->lock([raw](pipe_data& d) { d.parked = raw; });
        _io.submit(raw);
        return promise;
    }

    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline) override
    {
        if (!_open)
            return failed_async<cc::unit>({.code = error_code::connection_closed,
                                           .native_code = 0,
                                           .message = cc::string("the connection is closed")});

        if (_outbox->lock([](pipe_data const& d) { return d.writer_done; }))
            return failed_async<cc::unit>({.code = error_code::connection_closed,
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

    [[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> accept(deadline d) override
    {
        auto promise = cc::make_async_manual<cc::shared_ptr<tcp_connection>>();

        auto taken = _state->incoming.lock(
            [](cc::vector<cc::shared_ptr<tcp_connection>>& q) -> cc::shared_ptr<tcp_connection>
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
        _state->parked_accept.lock([raw](impl::io_operation*& slot) { slot = raw; });
        _net->io.submit(raw);
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

// ---- the network ---------------------------------------------------------------------------------------

virtual_network::virtual_network(io_system& io) : _state(cc::make_shared<state>(io))
{
}

virtual_network::~virtual_network() = default;

cc::result<cc::unique_ptr<tcp_listener>, error> virtual_network::listen(endpoint const& where,
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
    return cc::make_unique<tcp_listener>(std::make_unique<virtual_listener>(_state, cc::move(listener_state)));
}

cc::shared_async<cc::shared_ptr<tcp_connection>> virtual_network::connect(endpoint const& where,
                                                                          deadline /*d*/,
                                                                          tcp_options const& /*options*/)
{
    using handle = cc::shared_ptr<tcp_connection>;

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
        return failed_async<handle>({.code = error_code::connection_refused,
                                     .native_code = 0,
                                     .message = cc::format("nothing is listening on {}", where)});

    auto const client_endpoint = endpoint(where.address, _state->next_port.fetch_add(1));

    auto to_server = cc::make_shared<pipe>();
    auto to_client = cc::make_shared<pipe>();

    auto client = cc::make_shared<tcp_connection>(
        std::make_unique<virtual_connection>(_state->io, to_client, to_server, client_endpoint, where));
    auto accepted = cc::make_shared<tcp_connection>(
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
} // namespace cnet
