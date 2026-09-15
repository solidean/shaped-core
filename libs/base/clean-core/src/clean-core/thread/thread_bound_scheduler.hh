#pragma once

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/async_node.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh> // cc::thread_id

#if CC_HAS_THREADS
#include <condition_variable>
#include <mutex>
#endif

// cc::thread_bound_scheduler — a home that exactly one thread drains.
//
// A node homed here runs every segment of its frame on the owner thread: its first poll, every resume after a wake, every yield.
// Anything may submit from any thread; only the owner runs what was submitted, and only when it pumps or blocks.
// The main thread's instance is cc::main_thread_scheduler(), and cc::pump_main_thread() is the loop's call.
// The model is "Homes" in libs/base/clean-core/docs/systems/async.md.
//
// Without threads the queue is the same FIFO and the owner is the one thread there is; nothing is gated at the call site.

/// A scheduler drained by one owner thread.
///
/// Bind it with bind_to_current_thread() on the thread that will drain it, before homing anything to it.
/// It must be destroyed on that thread, after every node homed to it has resolved or been dropped.
struct cc::thread_bound_scheduler final : async_scheduler
{
    /// `default_inline_deps` is the policy a homed node's `home_default` resolves to.
    /// A thread home defaults to same-home-only: an owner thread is usually a latency or ownership constraint, not a throughput one.
    explicit thread_bound_scheduler(async_inline_deps default_inline_deps = async_inline_deps::same_home_only);
    ~thread_bound_scheduler() override;

    thread_bound_scheduler(thread_bound_scheduler const&) = delete;
    thread_bound_scheduler(thread_bound_scheduler&&) = delete;
    thread_bound_scheduler& operator=(thread_bound_scheduler const&) = delete;
    thread_bound_scheduler& operator=(thread_bound_scheduler&&) = delete;

    /// Makes the calling thread the owner.
    /// A thread owns at most one home, and a home is bound at most once.
    void bind_to_current_thread();

    [[nodiscard]] bool is_bound() const { return _owner != thread_id::invalid; }
    [[nodiscard]] bool is_owner_thread() const;

    /// Whether the calling thread is inside one of this home's own bodies, whose queue a wait there may not run.
    [[nodiscard]] bool is_inside_own_body() const { return _body_depth > 0 && is_owner_thread(); }

    // async_scheduler seam
public:
    /// A node reaches a thread home through submit; enqueue is only ever called for UNHOMED work scheduled while this
    /// home is bound, and forwards it to the compute scheduler — a home never inherits the work its bodies start.
    void enqueue(async_node_ptr node) override;
    void submit(async_node_ptr node) override;
    bool try_defer_teardown(async_node_base* node) override;

    /// Runs one queued item on the owner thread; false off the owner, when empty, or while one of this home's own bodies is running.
    bool try_run_one() override;

    /// Forwards to the compute scheduler: a home runs its own queue, never a graph's worth of unhomed work.
    void participate_until_ready(async_node_base& root) override;

    // pumping
public:
    /// Runs queued items until empty or `max_ms` of wall-clock elapses; max_ms <= 0 runs one bounded cycle.
    /// A cycle runs at most what was queued when it started, so a body that yields in a loop cannot pin the caller.
    /// The budget is checked between items, so one long body overruns it.
    /// Returns true if work is still queued.
    bool pump_for(double max_ms);

    /// Runs everything queued, including what that work queues in turn, until nothing is left.
    /// For the end of a loop: a deferred at_home teardown queued after the last pump is otherwise never run, and its node never freed.
    /// Owner thread only, and never from inside one of this home's bodies.
    void drain();

    /// One bounded cycle, for the blocking waits that service a home between their own steps.
    /// Stops between items once the steady clock passes `deadline_secs` (cc::current_time_steady_secs), leaving the rest queued first; 0 is no deadline.
    /// Returns true if anything ran.
    bool pump_cycle(double deadline_secs = 0);

    /// Whether anything is queued; racy by nature, for wait loops that re-check.
    [[nodiscard]] bool has_queued_work() const;

    /// Blocks the calling thread for at most `max_ms`, or until something is submitted; returns at once when work is queued.
    /// For the owner's own wait loops, which then pump.
    void wait_for_work(double max_ms);

    /// Blocks until something is submitted or wake() is called; returns at once when either already happened.
    /// For a blocking drive that registered its own reasons to be woken — see cc::impl::async_parker.
    /// `max_secs` < 0 waits without limit, which is every caller but a deadline-bounded one.
    void wait_for_work_or_wake(double max_secs = -1);

    /// Ends the owner's wait_for_work_or_wake, now or at its next call; from any thread.
    void wake();

    // internal
private:
    friend struct async_thread_pool;

    struct item
    {
        async_node_base* node = nullptr; // one strong count held by hand, or the collective weak for a teardown
        bool teardown = false;
    };

    void push(item it);
#if CC_HAS_THREADS
    /// Called by the pool the owner parked in, as it leaves; under _mutex, which is what lets push wake that pool safely.
    void clear_parked_in()
    {
        std::lock_guard const lock(_mutex);
        _parked_in.store(nullptr, cc::memory_order_relaxed);
    }
#endif
    bool take_one(item& out);
    void run(item it);

    thread_id _owner = thread_id::invalid;

    /// How many of this home's bodies are on the owner's stack; while > 0, try_run_one declines (a home is never re-entered).
    int _body_depth = 0;

    /// Owner-only FIFO consumed front to back; refilled from _incoming when exhausted.
    cc::vector<item> _local;
    isize _local_next = 0;

#if CC_HAS_THREADS
    mutable std::mutex _mutex;
    std::condition_variable _work_cv;
    cc::vector<item> _incoming;

    /// The pool the owner is parked in as a participant, or null — read, woken and cleared only under _mutex.
    cc::atomic<async_thread_pool*> _parked_in = {nullptr};

    /// Set by wake(), consumed by the next wait_for_work_or_wake; under _mutex.
    bool _wake_requested = false;
#endif
};

#if CC_HAS_THREADS
namespace cc::impl
{
/// Waits on `cv` for at most `secs`, or until notified.
/// Defined in thread.cc, the one place besides time.cc where <chrono> may be included.
void condition_wait_secs(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, double secs);
} // namespace cc::impl
#endif

namespace cc
{
/// The main thread's home: bound by cc::mark_current_thread_as_main(), and never destroyed.
/// Asserts if main was never marked.
[[nodiscard]] thread_bound_scheduler& main_thread_scheduler();

/// The event loop's call, on the main thread: runs the main home, one sweep of the pump registry, and — where the compute
/// and io schedulers have no threads of their own — steps those too.
///
/// Repeats until nothing progresses or `max_ms` elapses; max_ms <= 0 runs one cycle.
/// The budget is checked between the main home's items, so one long body overruns it.
/// Returns false only when nothing progressed and the main home has nothing queued, so a loop seeing false may wait.
bool pump_main_thread(double max_ms = 0);
} // namespace cc
