#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr

namespace sg::impl
{
/// Turns a backend's blocking GPU wait into settled completion asyncs, for backends whose API offers one.
///
/// `sg::context` only announces the lowest outstanding targets (`arm_completion_signal`); how completion is observed is the backend's business.
/// A backend that can park a thread on its GPU signals — dx12's fence events, vulkan's timeline semaphores — owns one of these and forwards `arm_completion_signal` to `arm`.
/// A backend whose API calls back instead, like WebGPU's `onSubmittedWorkDone`, needs none.
///
/// With threads a waiter thread parks on the armed targets, settles, and re-reads them.
/// Without threads it registers a pump instead, which parks only on work the GPU already has: the open epoch closes on an advance, and the thread that would advance is the one sweeping.
///
/// `stop` must run before anything the hooks touch is destroyed; the destructor stops as a backstop.
class completion_waiter
{
public:
    struct hooks
    {
        /// Parks until the submission timeline reaches `submission`, the epoch timeline reaches `epoch`, or `wake` is given a generation past `wake_generation`.
        /// A zero target is not waited on, and a spurious return is harmless.
        /// Only the waiter calls it, so it may keep per-waiter arming state without a lock.
        cc::unique_function<void(u64 submission, u64 epoch, u64 wake_generation)> park;

        /// Ends a `park` for an older generation; generations are strictly increasing.
        cc::unique_function<void(u64 generation)> wake;

        /// The owning context's `settle_due_completions`, which re-arms this waiter as targets change.
        cc::unique_function<void()> settle;

        /// The owning context's open epoch, which the no-threads pump must never park on.
        cc::unique_function<u64()> open_epoch;

        /// Whether the device is lost, after which no signal can be trusted to fire again.
        cc::unique_function<bool()> is_device_lost;
    };

    explicit completion_waiter(hooks h);
    ~completion_waiter();

    completion_waiter(completion_waiter const&) = delete;
    completion_waiter& operator=(completion_waiter const&) = delete;

    /// Records the lowest outstanding targets (0 where none), starting the waiter on the first real target.
    /// A target lower than the armed one wakes a parked waiter; a higher one needs no wake, since the waiter re-reads its targets after every settle.
    /// Safe from any thread.
    void arm(u64 submission, u64 epoch);

    /// Stops and joins the waiter, or drops the pump; idempotent.
    void stop();

private:
    struct state;
    std::unique_ptr<state> _state;
};
} // namespace sg::impl
