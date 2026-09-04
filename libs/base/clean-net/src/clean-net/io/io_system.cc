#include "io_system.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/native_socket.hh>
#include <clean-net/impl/reactor.hh>

namespace cnet::impl
{
namespace
{
struct submit_request
{
    io_operation* op = nullptr;
};

struct cancel_request
{
    io_operation* op = nullptr;
};

struct signal_request
{
    io_operation* op = nullptr;
};

using actor_handle = cc::threaded_actor<submit_request, cancel_request, signal_request>;

/// The reactor's semantic thread.
///
/// Every reactor call happens here, which is what lets reactor.hh take no lock: the mailbox is the serializer, so
/// `submit` from another thread is a message rather than a shared write.
class io_actor_impl final : public cc::threaded_actor_impl<submit_request, cancel_request, signal_request>
{
public:
    io_actor_impl(reactor& r, cc::atomic<isize>& pending, cc::atomic<bool>& stopping, i32 max_wait_ms, bool unthreaded)
      : _reactor(r), _pending(pending), _stopping(stopping), _max_wait_ms(max_wait_ms), _unthreaded(unthreaded)
    {
    }

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "cnet.io"; }

    void on_message(submit_request msg) override
    {
        _reactor.submit(msg.op);
        _pending.store(_reactor.pending_count());
    }

    void on_message(cancel_request msg) override
    {
        _reactor.cancel(msg.op);
        _pending.store(_reactor.pending_count());
    }

    void on_message(signal_request msg) override
    {
        _reactor.signal(msg.op);
        _pending.store(_reactor.pending_count());
    }

    bool on_process() override
    {
        // NOTHING COMPLETES ONCE THE IO_SYSTEM IS GOING AWAY.
        //
        // An unthreaded actor's shutdown drains synchronously on the caller, and that drain used to run one more
        // round of completions -- on the thread already inside `~io_system`, with the caller's transports and
        // fixtures partway through their own destructors.
        // A completion there calls back into user code holding references to objects that are already gone.
        //
        // `~reactor` says the same thing for the same reason: what is still pending is abandoned rather than
        // completed, because calling back into a half-destroyed program is worse than not calling back at all.
        // This is where that rule has to be enforced, since the actor gets to `on_process` first.
        if (_stopping.load())
            return false;

        if (_unthreaded)
        {
            // A pump registration must be cheap when it has nothing due: every blocking wait in the process sweeps
            // the whole registry, far more often than any one caller's own cadence.
            if (_reactor.pending_count() == 0)
                return false;

            auto const completed = _reactor.wait(0);
            _pending.store(_reactor.pending_count());
            return completed > 0;
        }

        // Threaded: this wait IS the sleep, so it always reports progress.
        //
        // Reporting none would send the actor loop to sleep on its inbox condition variable instead, where no socket
        // can ever wake it -- which is the one way a reactor built on this actor can stall.
        (void)_reactor.wait(_max_wait_ms);
        _pending.store(_reactor.pending_count());
        return true;
    }

private:
    reactor& _reactor;
    cc::atomic<isize>& _pending;
    cc::atomic<bool>& _stopping;
    i32 _max_wait_ms = 50;
    bool _unthreaded = false;
};
} // namespace

/// What io_system holds, so that threaded_actor.hh never reaches its header.
class io_actor
{
public:
    io_actor(cc::unique_ptr<reactor> r, clock& c, bool unthreaded)
      : _reactor(cc::move(r)), _clock(c), _unthreaded(unthreaded)
    {
    }

    ~io_actor() { stop(); }

    void stop()
    {
        if (!_handle.is_valid() || _stopping.exchange(true))
            return;

        // The actor goes down FIRST, so that by the time anything is settled below, no other thread is inside the
        // reactor: the thread is joined, or the unthreaded drain has run and the pump registration is gone.
        // That drain completes nothing, because `_stopping` is already set.
        //
        // The reactor is parked in a wait rather than on the actor's condition variable, so the actor's own shutdown
        // notification cannot reach it.
        // Waking it first turns a shutdown that takes one full wait into one that takes no time at all.
        _reactor->wake();
        _handle->shutdown();

        // Then answer everything outstanding, rather than leaving a caller holding an async that never settles --
        // which is what `submit` already does for an operation that arrives too late.
        // The continuations run inline from here, and every one of them finds `is_stopping()` true.
        _reactor->fail_all_pending(error{.code = error_code::cancelled,
                                         .native_code = 0,
                                         .message = cc::string("the io_system is shutting down")});
        _pending.store(0);
    }

    [[nodiscard]] bool is_stopping() const { return _stopping.load(); }

    void start(i32 max_wait_ms)
    {
        _handle = cc::make_threaded_actor<io_actor_impl>(*_reactor, _pending, _stopping, max_wait_ms, _unthreaded);
        _handle->start(_unthreaded ? cc::threaded_actor_mode::unthreaded : cc::threaded_actor_mode::threaded_if_possible);

        // The actor is the authority on what it became, and this is the one moment the two can be compared: the
        // answer had to be decided BEFORE start(), because the thread start() spawns can reach on_process before any
        // store here would land.
        // So the duplicate is a claim rather than a second source of truth, and this is what checks it.
        CC_ASSERT(_handle->is_unthreaded() == _unthreaded,
                  "the io_system and its actor disagree about whether there is a reactor thread");
    }

    void submit(io_operation* op)
    {
        if (!_handle->enqueue_message(submit_request{.op = op}))
        {
            // The actor is shutting down, so nothing will ever run this.
            // Completing it here rather than dropping it keeps the promise that every submitted operation is
            // answered exactly once.
            op->on_complete(error{.code = error_code::cancelled,
                                  .native_code = 0,
                                  .message = cc::string("the io_system is shutting down")});
            return;
        }
        _reactor->wake();
    }

    void cancel(io_operation* op)
    {
        if (_handle->enqueue_message(cancel_request{.op = op}))
            _reactor->wake();
    }

    void signal(io_operation* op)
    {
        if (_handle->enqueue_message(signal_request{.op = op}))
            _reactor->wake();
    }

    [[nodiscard]] isize pending_count() const { return _pending.load(); }
    [[nodiscard]] clock& time_source() const { return _clock; }
    [[nodiscard]] bool has_reactor_thread() const { return !_unthreaded; }

private:
    // Declared before the handle so it is destroyed after it: the actor thread must be joined before the reactor it
    // is waiting in goes away.
    cc::unique_ptr<reactor> _reactor;

    /// Readable from any thread, unlike the reactor's own count.
    cc::atomic<isize> _pending = 0;

    /// Set once teardown has begun, which is what stops the actor's drain from completing anything.
    cc::atomic<bool> _stopping = false;

    clock& _clock;
    bool _unthreaded = false;

    cc::unique_ptr<actor_handle> _handle;
};
} // namespace cnet::impl

namespace cnet
{
cc::result<cc::unique_ptr<io_system>, error> io_system::try_create(io_system_description const& desc)
{
    auto& c = desc.time_source != nullptr ? *desc.time_source : system_clock();

    auto reactor = impl::reactor::try_create(c);
    if (reactor.has_error())
        return cc::error(cc::move(reactor).error());

    // Decided before start() rather than read back from it: `io_actor_impl::on_process` branches on this, and the
    // thread start() spawns can reach it before a value stored afterwards would be visible.
    // `cc::threaded_actor::is_unthreaded()` is the authority and start() asserts the two agree; both terms below are
    // compile-time or startup facts, so there is nothing to race.
    //
    // A build without threads is unthreaded whatever the description says, exactly as cc::threaded_actor decides it.
    // So is a build without sockets: with nothing to wait on, a reactor thread could only spin -- and that one is not
    // a fact the actor knows.
    auto const unthreaded = desc.unthreaded || CC_HAS_THREADS == 0 || !impl::sockets_are_supported();

    auto system = cc::make_unique<io_system>();
    system->_actor = cc::make_unique<impl::io_actor>(cc::move(reactor).value(), c, unthreaded);
    system->_actor->start(desc.max_wait_ms);

    CC_LOG_TRACE("io_system up, {}", unthreaded ? "unthreaded" : "on its own thread");
    return system;
}

cc::unique_ptr<io_system> io_system::create(io_system_description const& desc)
{
    return try_create(desc).or_throw();
}

io_system::~io_system() = default;

void io_system::stop()
{
    _actor->stop();
}

bool io_system::is_stopping() const
{
    return _actor->is_stopping();
}

bool io_system::has_reactor_thread() const
{
    return _actor->has_reactor_thread();
}

clock& io_system::time_source() const
{
    return _actor->time_source();
}

void io_system::submit(impl::io_operation* op)
{
    _actor->submit(op);
}

void io_system::cancel(impl::io_operation* op)
{
    _actor->cancel(op);
}

void io_system::signal(impl::io_operation* op)
{
    _actor->signal(op);
}

isize io_system::pending_count() const
{
    return _actor->pending_count();
}

i64 deadline_to_absolute(io_system& io, deadline d)
{
    if (!d.is_finite())
        return 0;
    return io.time_source().now_ns() + d.timeout_ms * 1000 * 1000;
}
} // namespace cnet
