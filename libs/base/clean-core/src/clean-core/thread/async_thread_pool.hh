#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/math/random.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/impl/chase_lev_deque.hh>
#include <clean-core/thread/mutex.hh>

#if CC_HAS_THREADS

#include <condition_variable>
#include <mutex>
#include <thread>

// cc::async_thread_pool — a work-stealing scheduler that actually runs cc::async graphs concurrently.
//
// Each worker thread owns a lock-free Chase-Lev deque (see impl/chase_lev_deque.hh): it pushes and pops its own
// bottom end LIFO, so freshly spawned children stay hot and the common path takes no cross-thread sync at all;
// idle workers steal from the top of a sibling's deque, which is the only place threads meet. A shared,
// mutex-guarded injection queue takes work from foreign threads -- deliberately not lock-free, because only
// genuinely foreign submits PUSH to it (a worker waking a node enqueues locally). Note the asymmetry: its pop
// side is not cold at all, since every idle worker used to scan it, which is what _injection_hint is for. The
// pool implements the async_scheduler seam: schedule()/completion routing places nodes here (see async_node.hh).
//
//   cc::async_thread_pool pool;                           // hardware concurrency - 1; see the constructor
//   cc::install_default_async_pool(pool);                 // compute nodes now route here
//   auto a = cc::make_async_lazy([] { return heavy(); });
//   int v = pool.blocking_get(a);                         // drive on the pool, block THIS (foreign) thread
//
// blocking_get does NOT just hand the graph over and park: the calling thread borrows a slot and runs the graph
// itself, stealing like any worker, and only parks once there is genuinely nothing for it. A graph that never
// forks therefore touches no other thread at all (~27 ns, vs ~21 us for submit-and-park). That is also why the
// default worker count is one FEWER than the hardware concurrency -- the caller is the other thread.
//
// Lifetime: the pool must outlive every node routed to it (a woken node reaches its pool through the installed
// default, by design). The destructor stops the workers and joins them; it does not RUN outstanding work, so
// finish your graphs before tearing a pool down -- or abandon them deliberately, which is supported: queued
// work is released rather than leaked (see the drain in the destructor).
//
// Note the destructor cannot interrupt a running frame, and the eager depth-first drive means one frame can be
// most of a graph, so tearing down a pool mid-flight blocks until that unwinds. Workers only see the stop
// between tasks.

namespace cc
{
struct async_thread_pool final : async_scheduler
{
    /// Starts `worker_count` (>= 1) worker threads. Defaults to one FEWER than the hardware concurrency: a
    /// foreign thread in blocking_get participates as a worker for the duration, so the default leaves it a core.
    explicit async_thread_pool(int worker_count = default_worker_count());

    /// num_hardware_threads() - 1, floored at 1 — see the constructor on why it is not the full count.
    [[nodiscard]] static int default_worker_count();

    /// Stops and joins all workers. Asserts the pool is not still the installed default (uninstall it first —
    /// see uninstall_default_async_pool / scoped_default_async_pool). Does not drain queued work — outstanding
    /// graphs must have completed (or be intentionally abandoned).
    ~async_thread_pool() override;

    async_thread_pool(async_thread_pool const&) = delete;
    async_thread_pool(async_thread_pool&&) = delete;
    async_thread_pool& operator=(async_thread_pool const&) = delete;
    async_thread_pool& operator=(async_thread_pool&&) = delete;

    // async_scheduler seam
public:
    /// Local/hot enqueue onto the current worker's deque. Must be called from a worker of THIS pool (it is the
    /// route taken by a running frame scheduling a child / cold dependency).
    void enqueue(async_node_ptr node) override;

    /// Injection from any thread (foreign submits, cross-thread wakeups).
    void submit(async_node_ptr node) override;

    // queries
public:
    /// Worker THREADS. Excludes the external slots that foreign blocking_get callers borrow.
    [[nodiscard]] int worker_count() const { return _thread_count; }

    // blocking driver (call from a foreign thread — never from inside a worker/frame)
public:
    /// Drive `root` to completion and return its outcome. The calling thread PARTICIPATES — it borrows a pool
    /// slot and runs the graph itself rather than handing it over, so a small graph never leaves this thread;
    /// it only parks once there is nothing left for it to run. Asserts if called from within a worker of this
    /// pool (that would park a pool thread on its own work).
    template <class T, class E = async_error>
    [[nodiscard]] cc::result<T, E> try_blocking_get(shared_async<T, E> const& root);

    /// Like try_blocking_get but returns the value (copy) and asserts on error/cancellation.
    template <class T, class E = async_error>
    [[nodiscard]] T blocking_get(shared_async<T, E> const& root);

    // internal
private:
    struct worker
    {
        async_thread_pool* pool = nullptr;
        int id = 0;

        // Raw node pointers, each owning one strong count by hand (shared_ptr::release / adopt): a Chase-Lev
        // slot is read speculatively by thieves that may lose the race for it, so it cannot hold a smart handle.
        // The pool therefore owes every queued entry a release -- see the drain in ~async_thread_pool.
        cc::impl::chase_lev_deque<async_node_base*> deque;

        // Picks steal victims. Worker-private, so it needs no synchronization. Randomizing matters: a linear
        // scan points every idle worker at worker 0, which is both a contention hotspot and unfair.
        cc::random rng;

        std::thread thread; // empty on an external slot: those are driven by whichever foreign thread claims them

        // External slots only (index >= _thread_count). Foreign blocking_get callers claim one for the duration
        // of their drive; worker slots are never claimed.
        cc::atomic<bool> claimed{false};
    };

    void worker_main(worker& w);

    /// `authoritative` = this scan decides whether the caller may sleep, so it must not skip the injection queue
    /// (see the poller-token note on the member below). Cold path; the spin path passes false.
    [[nodiscard]] async_node_ptr try_get_work(worker& w, bool authoritative = false);
    void push_local(worker& w, async_node_ptr node);
    void wake_one();
    void wait_for_completion(async_node_base& root);

    // External participation: a foreign thread in blocking_get borrows a slot and runs the graph itself rather
    // than handing it over and parking. For a small graph that removes the submit/wake/wake round trip entirely
    // — the root lands in the caller's OWN deque and it polls it on the spot. Its deque is stealable like any
    // other, so a large graph still spreads across the pool.
    [[nodiscard]] worker* try_claim_external_slot();
    void participate_until_ready(async_node_base& root);
    void drain_slot_to_injection(worker& w);

    // Concurrent foreign callers are rare (usually one main thread), and an unclaimed slot only ever costs a
    // thief an empty probe. More would just dilute the steal rotation.
    static constexpr int external_slot_count = 4;

    // the worker whose loop is running on the calling thread (null on foreign threads); used by enqueue
    static thread_local worker* s_current_worker;

    // _workers holds _thread_count real workers followed by external_slot_count borrowable slots. Thieves scan
    // the whole vector: an unclaimed slot is simply an empty deque.
    cc::vector<cc::unique_ptr<worker>> _workers; // unique_ptr: stable addresses for deque/thread + stealing
    int _thread_count = 0;
    cc::mutex<cc::vector<async_node_ptr>> _injection;

    // The injection fast path, and the asymmetry it exists for: the queue is cold on the PUSH side (only foreign
    // threads submit) but its POP side sat on every idle worker's every scan — so N idle workers contended on
    // the one mutex a foreign submit needs, whether or not anything was ever in it.
    //
    //   _injection_hint   relaxed "may be non-empty". While it is 0 the line is read-only for every core and
    //                     sits Shared in each L1 — an L1 read, no traffic, same as _sleepers.
    //   _injection_poller grants ONE worker the right to look when the hint does go positive, so a submit wakes
    //                     a single poll rather than an N-way pile-up on the mutex.
    //
    // The hint is safe everywhere: the would-be sleeper's seq_cst fence orders it exactly as the protocol block
    // argues for _bottom. The POLLER TOKEN IS NOT — it is mutual exclusion, so it can make a scan miss work that
    // is really there. On a spin round that costs nothing (we loop again); on the pre-sleep re-scan it would
    // strand a node with every worker asleep. Hence try_get_work(authoritative=true) skips both filters.
    alignas(64) cc::atomic<int> _injection_hint{0};
    cc::atomic<int> _injection_poller{0};

    // The wake state. There is deliberately no counter of claimable tasks here: a worker's scan of the deques
    // already answers "is there work", authoritatively and without shared writes, so a counter would be a
    // hot-path RMW serving a cold-path question. See the protocol block in the .cc.
    alignas(64) cc::atomic<cc::i64> _wake_epoch{0}; // bumped only when a sleeper actually needs waking
    cc::atomic<int> _sleepers{0};                   // workers blocked on (or committing to) _wait_cv
    cc::atomic<bool> _stop{false};
    std::mutex _wait_m;
    std::condition_variable _wait_cv;
};

/// Install `pool` as the process-wide default: general-compute (bit 0) nodes that cannot run on the current
/// thread route here. Install once at startup, before the graphs that depend on it run. Asserts if a default
/// is already installed — overriding a live default is almost never correct (outer asyncs may outlive the
/// inner pool yet get scheduled on it). Pair with uninstall_default_async_pool, or use scoped_default_async_pool.
void install_default_async_pool(async_thread_pool& pool);

/// Remove `pool` as the process-wide default. Asserts it is the currently installed default. Must be called
/// before the pool is destroyed.
void uninstall_default_async_pool(async_thread_pool& pool);

/// RAII: installs `pool` as the process-wide default for the scope, uninstalling it on destruction.
struct scoped_default_async_pool
{
    explicit scoped_default_async_pool(async_thread_pool& pool) : _pool(pool) { install_default_async_pool(pool); }
    ~scoped_default_async_pool() { uninstall_default_async_pool(_pool); }

    scoped_default_async_pool(scoped_default_async_pool const&) = delete;
    scoped_default_async_pool(scoped_default_async_pool&&) = delete;
    scoped_default_async_pool& operator=(scoped_default_async_pool const&) = delete;
    scoped_default_async_pool& operator=(scoped_default_async_pool&&) = delete;

private:
    async_thread_pool& _pool;
};

// ============================================================================
// blocking driver — templated, defined inline
// ============================================================================

template <class T, class E>
cc::result<T, E> async_thread_pool::try_blocking_get(shared_async<T, E> const& root)
{
    CC_ASSERT(root != nullptr, "cannot drive a null async");
    CC_ASSERT(async_scheduler::current_or_null() != static_cast<async_scheduler*>(this),
              "do not call blocking_get from inside a worker of this pool (it would park a pool thread)");

    participate_until_ready(*root);

    CC_ASSERT(root->is_ready(), "async graph could not complete (blocked on external work?)");
    if (root->has_error())
        return cc::error(root->propagate_error());
    return *root->value_ptr(); // copy out
}

template <class T, class E>
T async_thread_pool::blocking_get(shared_async<T, E> const& root)
{
    auto r = try_blocking_get(root);
    CC_ASSERT(r.has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value();
}
} // namespace cc

#endif // CC_HAS_THREADS
