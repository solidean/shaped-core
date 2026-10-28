#include "datagram.hh"

#include <clean-core/error/exception.hh> // std::exception, through clean-core's one route to it
#include <clean-core/record/async_scope.hh>
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

        /// The record owner in effect where the callback was set, restored around each call.
        /// So what the callback records, and the error a throw from it logs, answer to that code rather than to the reactor thread.
        cc::rec::trace_id owner = {};

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

/// The arrival state this thread is installing a completion hook for.
/// A hook that fires during the install means the reactor thread finished the wait first, and the call is moved to the
/// reactor rather than run here, on whoever called `on_arrival`.
thread_local arrival_state const* t_hooking = nullptr;

/// Leaves `busy` on the way out, a throw included, so a clear on another thread is never left spinning.
struct busy_release
{
    arrival_state& a;
    ~busy_release() { a.busy.fetch_sub(1); }
};

/// Sets a thread-local for one scope and puts back what was there, a throw included.
struct thread_mark
{
    arrival_state const*& slot;
    arrival_state const* outer;

    thread_mark(arrival_state const*& s, arrival_state const* value) : slot(s), outer(s) { slot = value; }
    ~thread_mark() { slot = outer; }
};

void arm(cc::shared_ptr<arrival_state> const& a, u64 generation);

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
    auto owner = cc::rec::trace_id();
    auto const live = a->state.lock(
        [&](arrival_state::slots& s)
        {
            if (s.socket == nullptr || s.generation != generation || !s.callback.is_valid()
                || s.backend->io().is_stopping())
                return false;
            callback = s.callback;
            socket = s.socket;
            owner = s.owner;
            a->busy.fetch_add(1);
            return true;
        });
    if (!live)
        return;

    // Read before the call: a callback may destroy its own socket and then throw.
    auto const where = socket->local();
    {
        cc::rec::owner_scope const attributed(owner);
        auto const leave = busy_release{*a};
        auto const calling = thread_mark(t_calling, a.get());

        // Caught here, since nothing above does until the reactor's own loop, and that one ends the reactor thread.
        // A throw stops this callback only: not arming again is what the header promises.
        try
        {
            (*callback)(*socket);
        }
        catch (cc::exception const& e)
        {
            CC_LOG_ERROR("the arrival callback of the datagram socket on {} threw, and is not called again: {}", where,
                         e.message());
            return;
        }
        catch (std::exception const& e)
        {
            CC_LOG_ERROR("the arrival callback of the datagram socket on {} threw, and is not called again: {}", where,
                         e.what());
            return;
        }
        catch (...)
        {
            CC_LOG_ERROR("the arrival callback of the datagram socket on {} threw a non-std::exception value, and is "
                         "not "
                         "called again",
                         where);
            return;
        }
    }

    // With the generation this wait was armed for: a callback replaced meanwhile, here or on another thread, has armed
    // its own wait, and arming again would leave two.
    arm(a, generation);
}

/// Wait for the next arrival, unless the callback was replaced since `generation`, is gone, or the io_system is
/// stopping.
void arm(cc::shared_ptr<arrival_state> const& a, u64 generation)
{
    auto* backend = static_cast<datagram_backend*>(nullptr);
    auto token = cancel_token();
    auto previous = cancel_token();

    auto const go = a->state.lock(
        [&](arrival_state::slots& s)
        {
            if (s.socket == nullptr || s.generation != generation || !s.callback.is_valid()
                || s.backend->io().is_stopping())
                return false;
            previous = cc::move(s.pending);
            s.pending = cancel_token::create();
            token = s.pending;
            backend = s.backend;
            a->busy.fetch_add(1);
            return true;
        });
    if (!go)
        return;

    // Whatever was pending is not this wait's to keep; normally it already completed and this does nothing.
    previous.cancel();

    // Outside the lock: a backend is free to take its own locks and signal the reactor here.
    auto wait = cc::shared_async<cc::unit>();
    auto* io = static_cast<io_system*>(nullptr);
    {
        auto const leave = busy_release{*a};
        io = &backend->io();
        wait = backend->wait_readable(token);
    }

    auto const hooking = thread_mark(t_hooking, a.get());
    impl::when_ready(cc::move(wait),
                     [a, generation, io](cc::shared_async<cc::unit> const& w)
                     {
                         if (t_hooking == a.get())
                         {
                             // Finished before the hook was in: run it from the reactor, as every other call is.
                             impl::run_after(*io, 0, [a, generation, w] { on_readable(a, generation, w); });
                             return;
                         }
                         on_readable(a, generation, w);
                     });
}

/// Replace the callback, wait for a call of the old one still running on another thread, and say which generation
/// the new one is.
[[nodiscard]] u64 replace(arrival_state& a, cc::unique_function<void(datagram_socket&)> callback, bool socket_gone)
{
    auto generation = u64(0);
    auto stale = a.state.lock(
        [&](arrival_state::slots& s)
        {
            generation = ++s.generation;
            auto previous = cc::move(s.pending);
            s.callback = callback.is_valid()
                           ? cc::make_shared<cc::unique_function<void(datagram_socket&)>>(cc::move(callback))
                           : cc::shared_ptr<cc::unique_function<void(datagram_socket&)>>();
            s.owner = cc::rec::current_owner_id();
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

    return generation;
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
    (void)replace(*_arrival, {}, true);
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
    auto const generation = replace(*_arrival, cc::move(callback), false);
    if (watching)
        arm(_arrival, generation);
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
