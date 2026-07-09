#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>
#include <clean-core/thread/mutex.hh>

#if CC_HAS_THREADS

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// cc::async_thread_pool — a work-stealing scheduler that actually runs cc::async graphs concurrently.
//
// Each worker thread has a private LIFO deque (freshly spawned children stay hot) and steals from siblings
// when idle; a shared injection queue takes work from foreign threads and cross-affinity wakeups. The pool
// implements the async_scheduler seam: schedule()/completion routing places nodes here (see async_node.hh).
//
//   cc::async_thread_pool pool(cc::num_hardware_threads());
//   cc::install_default_async_pool(pool);                 // general-compute nodes now route here
//   auto a = cc::make_async_lazy([] { return heavy(); });
//   int v = pool.blocking_get(a);                         // drive on the pool, block THIS (foreign) thread
//
// Affinity: a pool serves one affinity mask (default general / bit 0). Every worker serves that mask, so a
// steal within a pool is always affinity-compatible. Nodes with a user-defined affinity (set via
// node->set_affinity) route to their own pool through their reschedule fn; construct one pool per task class
// and coexist them freely.
//
// Lifetime: the pool must outlive every node routed to it (nodes hold only a raw reschedule fn, by design).
// The destructor stops the workers and joins them; it does not drain outstanding work, so finish (or abandon)
// your graphs before tearing a pool down.

namespace cc
{
struct async_thread_pool final : async_scheduler
{
    /// Starts `worker_count` (>= 1) worker threads, each serving `served` (default general-purpose compute).
    explicit async_thread_pool(int worker_count, async_affinity served = async_affinity::general());

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
    void enqueue(std::shared_ptr<async_node_base> node) override;

    /// Affinity-routed injection from any thread (foreign submits, cross-affinity wakeups). Asserts the node's
    /// affinity overlaps this pool's served mask.
    void submit(std::shared_ptr<async_node_base> node) override;

    // queries
public:
    [[nodiscard]] async_affinity served() const { return _served; }
    [[nodiscard]] int worker_count() const { return int(_workers.size()); }

    // blocking driver (call from a foreign thread — never from inside a worker/frame)
public:
    /// Submit `root` to this pool and block the calling thread until it completes, returning its outcome.
    /// Asserts if called from within a worker of this pool (that would park a pool thread on its own work).
    template <class T>
    [[nodiscard]] cc::result<T, async_error> try_blocking_get(shared_async<T> const& root);

    /// Like try_blocking_get but returns the value (copy) and asserts on error/cancellation.
    template <class T>
    [[nodiscard]] T blocking_get(shared_async<T> const& root);

    // internal
private:
    struct worker
    {
        async_thread_pool* pool = nullptr;
        int id = 0;
        async_affinity served;
        cc::mutex<cc::vector<std::shared_ptr<async_node_base>>> deque;
        std::thread thread;
    };

    void worker_main(worker& w);
    [[nodiscard]] std::shared_ptr<async_node_base> try_get_work(worker& w);
    void push_local(worker& w, std::shared_ptr<async_node_base> node);
    void wake_one();
    void block_until_ready(async_node_base& root);

    // the worker whose loop is running on the calling thread (null on foreign threads); used by enqueue
    static thread_local worker* s_current_worker;

    cc::vector<cc::unique_ptr<worker>> _workers; // unique_ptr: stable addresses for deque/thread + stealing
    cc::mutex<cc::vector<std::shared_ptr<async_node_base>>> _injection;
    async_affinity _served;

    std::atomic<int> _pending{0};  // claimable tasks across all deques + injection (drives sleep/wake)
    std::atomic<int> _sleepers{0}; // workers currently blocked on _wait_cv
    std::atomic<bool> _stop{false};
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

template <class T>
cc::result<T, async_error> async_thread_pool::try_blocking_get(shared_async<T> const& root)
{
    CC_ASSERT(root != nullptr, "cannot drive a null async");
    CC_ASSERT(async_scheduler::current_or_null() != static_cast<async_scheduler*>(this),
              "do not call blocking_get from inside a worker of this pool (it would park a pool thread)");

    block_until_ready(*root);

    CC_ASSERT(root->is_ready(), "async graph could not complete (blocked on external work?)");
    if (root->has_error())
        return cc::error(root->propagate_error());
    return *root->value_ptr(); // copy out
}

template <class T>
T async_thread_pool::blocking_get(shared_async<T> const& root)
{
    auto r = try_blocking_get(root);
    CC_ASSERT(r.has_value(), "async completed with an error or was cancelled");
    return cc::move(r).value();
}
} // namespace cc

#endif // CC_HAS_THREADS
