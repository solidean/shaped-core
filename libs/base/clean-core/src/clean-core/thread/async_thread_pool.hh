#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/random.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/impl/async_tls.hh>
#include <clean-core/thread/mutex.hh>

#if CC_HAS_THREADS
#include <clean-core/thread/impl/chase_lev_deque.hh>

#include <condition_variable>
#include <mutex>
#include <thread>
#endif

// cc::async_thread_pool — a work-stealing scheduler that actually runs cc::async graphs concurrently.
// It implements the async_scheduler seam, so schedule() and completion routing place nodes here (see async_node.hh).
// The design and the wake protocol are documented in libs/base/clean-core/docs/systems/async.md.
// The measured scaling, and the four benchmarks behind it, are in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
//
//   cc::async_thread_pool pool;                           // hardware concurrency - 1; see the constructor
//   cc::scoped_default_async_scheduler const ambient(pool); // every async now belongs to this pool
//   auto a = cc::make_async_lazy([] { return heavy(); });
//   int v = cc::async_blocking_get(a);                    // drive on the ambient scheduler, block THIS thread
//
// Each worker owns a lock-free Chase-Lev deque (impl/chase_lev_deque.hh) and idle workers steal from a sibling's.
// The shared, mutex-guarded injection queue is asymmetric: its PUSH side is cold, since only genuinely foreign
// submits reach it, but its POP side sat on every idle worker's scan -- which is what _injection_hint below is for.
//
// Participation does NOT hand the graph over and park: the calling thread borrows a slot and runs the graph
// itself, stealing like any worker, and parks only once there is genuinely nothing left for it.
//
// Lifetime: the pool must outlive every node routed to it, since a woken node reaches its pool through the
// installed default by design.
// The destructor stops the workers and joins them; it does not RUN outstanding work, so finish your graphs
// before tearing a pool down.
// Abandoning them deliberately is supported -- queued work is released rather than leaked (see the drain in the destructor).
// The destructor cannot interrupt a running frame, and the eager depth-first drive means one frame can be most
// of a graph, so tearing a pool down mid-flight blocks until that unwinds.
//
// WITHOUT THREADS (CC_HAS_THREADS == 0 — WASM, or -DSC_THREADS=OFF) the pool keeps its entire API and falls
// back rather than disappearing, so calling code stays identical across platforms.
// It starts no threads and becomes cc::singlethreaded_scheduler wearing the pool's API — see "Without threads"
// in libs/base/clean-core/docs/systems/async.md.
// What it cannot do is wait: a graph parked on work only another thread could supply never completes here, so a
// driver keeps asking for progress that can never come.

struct cc::async_thread_pool final : async_scheduler
{
    /// Starts `worker_count` (>= 1) worker threads.
    /// Defaults to one FEWER than the hardware concurrency: a thread driving a graph participates as a worker for the duration, so the default leaves it a core.
    /// Without threads nothing is started and the count is ignored.
    explicit async_thread_pool(int worker_count = default_worker_count());

    /// cc::recommended_worker_count() - 1, floored at 1 — see the constructor on why it is not the full count.
    /// 0 without threads: whoever drives a graph is the only worker there ever is.
    [[nodiscard]] static int default_worker_count();

    /// Stops and joins all workers.
    /// Asserts the pool is not still the installed default — uninstall it first, via uninstall_default_async_scheduler or scoped_default_async_scheduler.
    /// Does not drain queued work: outstanding graphs must have completed, or be intentionally abandoned.
    ~async_thread_pool() override;

    async_thread_pool(async_thread_pool const&) = delete;
    async_thread_pool(async_thread_pool&&) = delete;
    async_thread_pool& operator=(async_thread_pool const&) = delete;
    async_thread_pool& operator=(async_thread_pool&&) = delete;

    // async_scheduler seam
public:
    /// Local/hot enqueue onto the current worker's deque.
    /// Must be called from a worker of THIS pool — it is the route a running frame takes when scheduling a child or a cold dependency.
    /// Without threads there are no workers and no such restriction: it queues onto the pump whoever drives a graph will run.
    void enqueue(async_node_ptr node) override;

    /// Injection from any thread (foreign submits, cross-thread wakeups).
    void submit(async_node_ptr node) override;

    /// Runs one node from the calling worker's own deque.
    /// False on any other thread: running a pool's work needs a slot to run it in, and a foreign caller takes one by participating instead.
    bool try_run_one() override;

    /// Drives `root` to completion on the calling thread.
    ///
    /// The caller PARTICIPATES: it borrows a pool slot and runs the graph itself rather than handing it over, so a small graph never leaves this thread.
    /// It parks on the root only once there is nothing left for it to run, which is why it returns ready even for a graph finished by an external push.
    /// Legal from inside a worker — cc::async_blocking_get bridges sync and async code, and a nested drive keeps that thread working rather than idle.
    /// Without threads the caller is not merely a participant but the only one, and runs the whole graph inline.
    void participate_until_ready(async_node_base& root) override;

    // queries
public:
    /// Worker THREADS, excluding the external slots that participating foreign threads borrow.
    /// 0 without threads: whoever drives is the only worker there is.
    [[nodiscard]] int worker_count() const { return _thread_count; }

    // internal
private:
#if CC_HAS_THREADS
    struct worker
    {
        async_thread_pool* pool = nullptr;
        int id = 0;

        // Raw node pointers, each owning one strong count by hand via shared_ptr::release / adopt.
        // A Chase-Lev slot is read speculatively by thieves that may lose the race for it, so it cannot hold a smart handle.
        // The pool therefore owes every queued entry a release -- see the drain in ~async_thread_pool.
        cc::impl::chase_lev_deque<async_node_base*> deque;

        // Picks steal victims; worker-private, so it needs no synchronization.
        // Randomizing matters: a linear scan points every idle worker at worker 0, which is both a contention hotspot and unfair.
        cc::random rng;

        std::thread thread; // empty on an external slot: those are driven by whichever foreign thread claims them

        // External slots only (index >= _thread_count).
        // A foreign thread participating claims one for the duration of its drive; worker slots are never claimed.
        cc::atomic<bool> claimed = {false};
    };

    void worker_main(worker& w);

    /// `authoritative` = this scan decides whether the caller may sleep, so it must not skip the injection queue (see the poller-token note on the member below).
    /// Cold path; the spin path passes false.
    [[nodiscard]] async_node_ptr try_get_work(worker& w, bool authoritative = false);
    void push_local(worker& w, async_node_ptr node);
    void wake_one();
    void wake_all();

    // External participation: a foreign thread borrows a slot and runs the graph itself rather than handing it over and parking.
    // For a small graph that removes the submit/wake round trip entirely — the root lands in the caller's OWN deque and it polls it on the spot.
    // Its deque is stealable like any other, so a large graph still spreads across the pool.
    [[nodiscard]] worker* try_claim_external_slot();
    void drain_slot_to_injection(worker& w);

    // Concurrent foreign callers are rare, usually one main thread, and an unclaimed slot only ever costs a thief an empty probe.
    // More would just dilute the steal rotation.
    static constexpr int external_slot_count = 4;

    // The worker whose loop is running on the calling thread (null on foreign threads); used by enqueue.
    // Stored in the shared per-thread block (impl/async_tls.hh) as a void*, so a poll resolves ONE TLS address for
    // the scheduler, the inline depth and this — hence the cast pair rather than a typed slot.
    [[nodiscard]] static worker* current_worker() { return static_cast<worker*>(cc::impl::async_tls().current_worker); }
    static void set_current_worker(worker* w) { cc::impl::async_tls().current_worker = w; }

    // _workers holds _thread_count real workers followed by external_slot_count borrowable slots.
    // Thieves scan the whole vector: an unclaimed slot is simply an empty deque.
    cc::vector<cc::unique_ptr<worker>> _workers; // unique_ptr: stable addresses for deque/thread + stealing
    int _thread_count = 0;
    cc::mutex<cc::vector<async_node_ptr>> _injection;

    // The injection fast path, and the asymmetry it exists for: the queue is cold on the PUSH side, since only
    // foreign threads submit, but its POP side sat on every idle worker's every scan.
    // N idle workers therefore contended on the one mutex a foreign submit needs, whether or not anything was ever in it.
    //
    //   _injection_hint   relaxed "may be non-empty". While it is 0 the line is read-only for every core and
    //                     sits Shared in each L1 — an L1 read, no traffic, same as _sleepers.
    //   _injection_poller grants ONE worker the right to look when the hint does go positive, so a submit wakes
    //                     a single poll rather than an N-way pile-up on the mutex.
    //
    // The hint is safe everywhere: the would-be sleeper's seq_cst fence orders it exactly as the protocol block argues for _bottom.
    // The POLLER TOKEN IS NOT — it is mutual exclusion, so it can make a scan miss work that is really there.
    // On a spin round that costs nothing, since we loop again; on the pre-sleep re-scan it would strand a node with every worker asleep.
    // Hence try_get_work(authoritative=true) skips both filters.
    alignas(64) cc::atomic<int> _injection_hint = {0};
    cc::atomic<int> _injection_poller = {0};

    // The wake state.
    // There is deliberately no counter of claimable tasks: a worker's scan of the deques already answers "is there work", authoritatively and without shared writes.
    // A counter would be a hot-path RMW serving a cold-path question — see the protocol block in the .cc.
    alignas(64) cc::atomic<i64> _wake_epoch = {0}; // bumped only when a sleeper actually needs waking
    cc::atomic<int> _sleepers = {0};               // workers blocked on (or committing to) _wait_cv
    cc::atomic<bool> _stop = {false};
    std::mutex _wait_m;
    std::condition_variable _wait_cv;

#else // CC_HAS_THREADS == 0

    // The whole pool without threads.
    // None of the machinery above has anything to do: work-stealing needs thieves, and the wake protocol exists to park and wake threads that do not exist.
    // What is left is the queue, held as real handles, so it frees itself — unlike the threaded deques' hand-counted raw pointers.
    //
    // LIFO, matching singlethreaded_scheduler and the workers' own deques: a freshly spawned child is the hottest thing in cache, and running it next is what makes the drive depth-first.
    cc::vector<async_node_ptr> _queue;
    int _thread_count = 0; // never anything else; kept so worker_count() reads the same either way

#endif
};
