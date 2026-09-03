#include "io_system.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <clean-net/fwd.hh>
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

using actor_handle = cc::threaded_actor<submit_request, cancel_request>;

/// The reactor's semantic thread.
///
/// Every reactor call happens here, which is what lets reactor.hh take no lock: the mailbox is the serializer, so
/// `submit` from another thread is a message rather than a shared write.
class io_actor_impl final : public cc::threaded_actor_impl<submit_request, cancel_request>
{
public:
    io_actor_impl(reactor& r, cc::atomic<isize>& pending, i32 max_wait_ms, bool unthreaded)
      : _reactor(r), _pending(pending), _max_wait_ms(max_wait_ms), _unthreaded(unthreaded)
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

    bool on_process() override
    {
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

    ~io_actor()
    {
        if (!_handle.is_valid())
            return;

        // The reactor is parked in a wait rather than on the actor's condition variable, so the actor's own shutdown
        // notification cannot reach it.
        // Waking it first turns a shutdown that takes one full wait into one that takes no time at all.
        _reactor->wake();
        _handle->shutdown();
    }

    void start(i32 max_wait_ms)
    {
        _handle = cc::make_threaded_actor<io_actor_impl>(*_reactor, _pending, max_wait_ms, _unthreaded);
        _handle->start(_unthreaded ? cc::threaded_actor_mode::unthreaded : cc::threaded_actor_mode::threaded_if_possible);
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

    [[nodiscard]] isize pending_count() const { return _pending.load(); }
    [[nodiscard]] clock& time_source() const { return _clock; }
    [[nodiscard]] bool has_reactor_thread() const { return !_unthreaded; }

private:
    // Declared before the handle so it is destroyed after it: the actor thread must be joined before the reactor it
    // is waiting in goes away.
    cc::unique_ptr<reactor> _reactor;

    /// Readable from any thread, unlike the reactor's own count.
    cc::atomic<isize> _pending = 0;

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

    // Decided before start() rather than read back from it, so no thread can run on_process before the answer is in.
    // A build without threads is unthreaded whatever the description says, exactly as cc::threaded_actor decides it.
    auto const unthreaded = desc.unthreaded || CC_HAS_THREADS == 0;

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

isize io_system::pending_count() const
{
    return _actor->pending_count();
}
} // namespace cnet
