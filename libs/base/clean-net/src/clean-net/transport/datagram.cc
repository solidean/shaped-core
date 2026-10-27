#include "datagram.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/native_socket.hh>
#include <clean-net/transport/stream.hh>

// A datagram socket is two halves that share nothing but the backend.
//
// THE BATCH CALLS are plain synchronous calls on the caller's thread: they never touch the reactor, which is what keeps
// a game polling once per frame at the cost of a syscall and nothing else.
//
// THE ARRIVAL CALLBACK is a loop of one-shot readiness waits: wait, call, wait again.
// Each wait is a reactor operation, so it is one completion per wake rather than per datagram -- a burst of fifty
// costs one call that drains fifty.

namespace cnet
{
/// The callback, and what keeps it from outliving the socket it is about.
///
/// Shared with every wait in flight, since a wait can complete after the socket is gone; `socket` going null is how it
/// learns that.
struct datagram_socket::arrival_state
{
    struct slots
    {
        /// Null once the socket is destroyed.
        datagram_socket* socket = nullptr;
        datagram_backend* backend = nullptr;

        /// Shared so a call can run outside the lock while a replacement is installed.
        cc::shared_ptr<cc::unique_function<void(datagram_socket&)>> callback;

        /// Bumped by every replace and clear, so a wait armed for an older callback does nothing when it lands.
        u64 generation = 0;

        /// Cancels the one wait in flight.
        cancel_token pending;
    };

    cc::mutex<slots> state;

    /// Calls and arms in progress on any thread, which a clear waits out before the backend may go away.
    cc::atomic<i32> busy = 0;
};

namespace
{
using arrival_state = datagram_socket::arrival_state;

/// The arrival state whose callback this thread is running, so a clear from inside the callback does not wait for
/// itself.
thread_local arrival_state const* t_calling = nullptr;

void arm(cc::shared_ptr<arrival_state> const& a);

void on_readable(cc::shared_ptr<arrival_state> const& a, u64 generation, cc::shared_async<cc::unit> const& wait)
{
    if (wait->has_error())
    {
        // Cancelled is the normal end: the callback was replaced or cleared, or the io_system is stopping.
        auto const failure = wait->propagate_error();
        if (!failure.is_cancelled())
            CC_LOG_WARNING("a datagram socket stopped watching for arrivals: {}", failure.underlying().to_string());
        return;
    }

    auto callback = cc::shared_ptr<cc::unique_function<void(datagram_socket&)>>();
    auto* socket = static_cast<datagram_socket*>(nullptr);
    auto const live = a->state.lock(
        [&](arrival_state::slots& s)
        {
            if (s.socket == nullptr || s.generation != generation || !s.callback.is_valid())
                return false;
            callback = s.callback;
            socket = s.socket;
            a->busy.fetch_add(1);
            return true;
        });
    if (!live)
        return;

    auto const* const outer = t_calling;
    t_calling = a.get();
    (*callback)(*socket);
    t_calling = outer;
    a->busy.fetch_sub(1);

    arm(a);
}

/// Wait for the next arrival, unless the callback is gone or the io_system is stopping.
void arm(cc::shared_ptr<arrival_state> const& a)
{
    auto* backend = static_cast<datagram_backend*>(nullptr);
    auto token = cancel_token();
    auto generation = u64(0);

    auto const go = a->state.lock(
        [&](arrival_state::slots& s)
        {
            if (s.socket == nullptr || !s.callback.is_valid() || s.backend->io().is_stopping())
                return false;
            s.pending = cancel_token::create();
            token = s.pending;
            backend = s.backend;
            generation = s.generation;
            a->busy.fetch_add(1);
            return true;
        });
    if (!go)
        return;

    // Outside the lock: a backend is free to take its own locks and signal the reactor here.
    // Never inline, which the backend promises, so the continuation cannot run before `busy` drops.
    auto wait = backend->wait_readable(token);
    a->busy.fetch_sub(1);

    impl::when_ready(cc::move(wait),
                     [a, generation](cc::shared_async<cc::unit> const& w) { on_readable(a, generation, w); });
}

/// Replace the callback, and wait for a call of the old one still running on another thread.
void replace(arrival_state& a, cc::unique_function<void(datagram_socket&)> callback, bool socket_gone)
{
    auto stale = a.state.lock(
        [&](arrival_state::slots& s)
        {
            ++s.generation;
            auto previous = cc::move(s.pending);
            s.callback = callback.is_valid()
                           ? cc::make_shared<cc::unique_function<void(datagram_socket&)>>(cc::move(callback))
                           : cc::shared_ptr<cc::unique_function<void(datagram_socket&)>>();
            if (socket_gone)
            {
                s.socket = nullptr;
                s.backend = nullptr;
            }
            return previous;
        });
    stale.cancel();

    // A call already running elsewhere still holds the old callback and the socket reference; from inside the callback
    // itself there is nothing to wait for, and waiting would never end.
    if (t_calling != &a)
        while (a.busy.load() > 0)
            cc::this_thread_yield();
}

// ---- the native backend --------------------------------------------------------------------------------

/// A readiness wait on a native socket: a reactor `readable` operation, which completes without reading.
struct readable_operation final : impl::io_operation
{
    cc::shared_async<cc::unit> promise;
    cc::unique_ptr<readable_operation> self;
    impl::cancel_registration cancellation;

    /// The reactor watches this socket by handle, so the operation keeps it alive while it does.
    cc::shared_ptr<impl::socket_holder> socket_owner;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        if (failure.has_value())
            promise->push_error(to_async_error(cc::move(failure.value())));
        else
            promise->push_value(cc::unit{});
    }
};

class native_datagram_backend final : public datagram_backend
{
public:
    native_datagram_backend(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint)
      : _io(io), _socket(cc::move(s)), _local(local_endpoint)
    {
    }

    [[nodiscard]] cc::result<isize, error> receive_batch(cc::span<datagram> records) override
    {
        return impl::receive_datagrams(_socket->handle, records, _io.time_source());
    }

    [[nodiscard]] cc::result<isize, error> send_batch(cc::span<outgoing_datagram const> packets) override
    {
        return impl::send_datagrams(_socket->handle, packets);
    }

    [[nodiscard]] cc::shared_async<cc::unit> wait_readable(cancel_token const& token) override
    {
        auto op = cc::make_unique<readable_operation>();
        op->kind = impl::io_op_kind::readable;
        op->socket = _socket->handle;
        op->socket_owner = _socket;

        auto promise = cc::make_async_manual<cc::unit>();
        op->promise = promise;

        auto* const raw = op.get();
        raw->self = cc::move(op);
        raw->cancellation.attach(_io.submit(raw), token);
        return promise;
    }

    [[nodiscard]] endpoint local() const override { return _local; }
    [[nodiscard]] io_system& io() const override { return _io; }

private:
    io_system& _io;
    cc::shared_ptr<impl::socket_holder> _socket;
    endpoint _local;
};
} // namespace

// ---- the handle ----------------------------------------------------------------------------------------

datagram_socket::datagram_socket(std::unique_ptr<datagram_backend> backend)
  : _backend(cc::move(backend)), _arrival(cc::make_shared<arrival_state>())
{
    _arrival->state.lock(
        [this](arrival_state::slots& s)
        {
            s.socket = this;
            s.backend = _backend.get();
        });
}

datagram_socket::~datagram_socket()
{
    replace(*_arrival, {}, true);
}

cc::result<isize, error> datagram_socket::receive_batch(cc::span<datagram> records)
{
    return _backend->receive_batch(records);
}

cc::result<isize, error> datagram_socket::send_batch(cc::span<outgoing_datagram const> packets)
{
    return _backend->send_batch(packets);
}

void datagram_socket::on_arrival(cc::unique_function<void(datagram_socket&)> callback)
{
    auto const watching = callback.is_valid();
    replace(*_arrival, cc::move(callback), false);
    if (watching)
        arm(_arrival);
}

endpoint datagram_socket::local() const
{
    return _backend->local();
}

cc::result<cc::unique_ptr<datagram_socket>, error> datagram_socket::try_create(io_system& io,
                                                                               endpoint const& where,
                                                                               datagram_options const& options)
{
    auto native = native_transport(io);
    return native.bind_datagram(where, options);
}

cc::result<cc::unique_ptr<datagram_socket>, error> datagram_socket::try_create(transport& t,
                                                                               endpoint const& where,
                                                                               datagram_options const& options)
{
    return t.bind_datagram(where, options);
}

cc::unique_ptr<datagram_socket> datagram_socket::create(io_system& io,
                                                        endpoint const& where,
                                                        datagram_options const& options)
{
    return try_create(io, where, options).or_throw();
}

// ---- the native transport ------------------------------------------------------------------------------

cc::result<cc::unique_ptr<datagram_socket>, error> native_transport::bind_datagram(endpoint const& where,
                                                                                   datagram_options const& options)
{
    if (!impl::sockets_are_supported())
        return cc::error(unsupported_here("binding a datagram socket"));
    if (!where.address.is_valid())
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("bind: the endpoint has no address")});

    auto created = impl::create_udp_socket(where.address.family());
    if (created.has_error())
        return cc::error(cc::move(created).error());

    // Owned from here, so every failure below closes it by returning.
    auto holder = cc::make_shared<impl::socket_holder>(created.value());
    auto const s = holder->handle;

    if (auto applied = impl::apply_datagram_options(s, where.address.family(), options); applied.has_error())
        return cc::error(cc::move(applied).error());

    if (auto bound = impl::bind_socket(s, where, options.reuse_address); bound.has_error())
        return cc::error(cc::move(bound).error());

    auto local = impl::local_endpoint(s);
    if (local.has_error())
        return cc::error(cc::move(local).error());

    CC_LOG_TRACE("datagram socket on {}", local.value());
    return cc::make_unique<datagram_socket>(
        std::make_unique<native_datagram_backend>(_io, cc::move(holder), local.value()));
}
} // namespace cnet
