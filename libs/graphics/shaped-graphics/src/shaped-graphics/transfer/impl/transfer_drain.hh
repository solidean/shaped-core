#pragma once

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/fwd.hh> // std::shared_ptr

/// How a transfer system answers "have you delivered everything you were given?".
///
/// The question is not "is your inbox empty":
/// An actor with a thread of its own reports no pumpable work while it is still busy, and one that keeps receiving
/// messages would never satisfy an inbox-equality test at all — so what is counted is OUTSTANDING WORK, and a waiter
/// leaves as soon as that reaches zero rather than when the actor goes quiet.
///
/// A job holds a `transfer_drain::token`, and the count drops when that token is destroyed.
/// Tying it to the job's LIFETIME rather than to its delivery is what makes it correct on every exit path at once:
/// delivered, cancelled, dropped unsubmitted, or abandoned at shutdown.
class sg::impl::transfer_drain
{
public:
    /// Held by one in-flight job.
    /// Copyable, because a job is moved between queues and windows on its way through.
    using token = std::shared_ptr<void>;

    /// Count one job, and hand back the token whose destruction uncounts it.
    [[nodiscard]] token start()
    {
        _outstanding.fetch_add(1, cc::memory_order_relaxed);
        return token(this, [](void* self) { static_cast<transfer_drain*>(self)->finish(); });
    }

    /// Whether nothing is in flight right now.
    [[nodiscard]] bool is_idle() const { return _outstanding.load(cc::memory_order_acquire) == 0; }

    /// Blocks until every job counted so far has been destroyed.
    ///
    /// Only the actor drops the last reference, so where it has no thread of its own we must RUN it rather than wait
    /// on it — which is what the pump is for, and why it is tried before anything blocks.
    void wait_until_idle()
    {
        while (_outstanding.load(cc::memory_order_acquire) != 0)
        {
            if (cc::thread_pump_all())
                continue; // ran some of it; look again

#if CC_HAS_THREADS
            // Nothing to run here, so the rest belongs to an actor thread: park until it says otherwise.
            // Re-read under the wait, which rechecks — a drop to zero between the load and the wait is not lost.
            auto const cur = _outstanding.load(cc::memory_order_acquire);
            if (cur != 0)
                _outstanding.wait(cur, cc::memory_order_acquire);
#else
            // Without threads the pump IS the actor, so "nothing left to run" means nothing left that could finish.
            // Waiting would be a deadlock against the one thread that exists — see clean-core/thread/atomic.hh.
            break;
#endif
        }
    }

private:
    void finish()
    {
        if (_outstanding.fetch_sub(1, cc::memory_order_acq_rel) == 1)
        {
#if CC_HAS_THREADS
            _outstanding.notify_all();
#endif
        }
    }

    cc::atomic<isize> _outstanding = 0;
};
