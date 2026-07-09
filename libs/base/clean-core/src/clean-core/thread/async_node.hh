#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/function/function_ref.hh>
#include <clean-core/fwd.hh>

#include <atomic>
#include <memory> // std::unique_ptr — polymorphic child ownership (cc::unique_ptr has no upcast)

// Untemplated core of the cc::async dataflow system: the node state machine, the pending-dependency /
// continuation bookkeeping, the scheduler seam, and the failure-channel value type. The templated public
// surface (async<T>, once_async<T>, async_result<T>, make_async_*, async_blocking_get) lives in async.hh.
//
// Nothing here ever blocks a thread: poll() drives a node's compute frame forward until it completes, fails
// as a value, or parks on not-ready dependencies with wakeup continuations installed. The default driver
// runs everything inline on the calling thread (see inline_scheduler), which is both the shipped default and
// what makes the system testable without threads. A real work-stealing pool is a future layer on the same
// async_scheduler seam.

namespace cc
{
// ============================================================================
// affinity + reschedule routing (which worker may run a node; where a woken node goes)
// ============================================================================

/// A typed task-class bitmask. A node may run on a worker thread iff their masks overlap (bitwise AND != 0).
/// Bit 0 is general-purpose compute — the default for every compute ("pull") async and for a worker scope
/// with no explicit mask. Higher bits are user-defined task classes. A push/manual async carries the empty
/// mask (nothing to run). Work stealing is permitted only between overlapping masks.
struct async_affinity
{
    cc::u32 bits = 1; // bit 0 = general-purpose compute

    [[nodiscard]] static async_affinity general() { return {1u}; }
    [[nodiscard]] static async_affinity none() { return {0u}; }

    [[nodiscard]] bool overlaps(async_affinity o) const { return (bits & o.bits) != 0; }
    [[nodiscard]] bool is_empty() const { return bits == 0; }

    friend bool operator==(async_affinity, async_affinity) = default;
};

/// Routes a (re)scheduled node to the scheduler/pool serving its affinity. A raw function pointer (not a
/// unique_function): compute nodes are long-lived and routing must stay allocation-free. Invoked from the
/// completion/wakeup path, possibly on a foreign thread, so it must not depend on any thread-local worker
/// scope. The referenced pool must outlive every node routed to it (caller's responsibility).
using async_reschedule_fn = void (*)(std::shared_ptr<async_node_base>);

/// The default route for general-compute (bit 0) nodes: the installed default scheduler if one exists, else
/// the current thread's worker scope (inline driving), else an assert. Compute nodes take this route by
/// default; set_affinity replaces it for a user-defined task class.
void async_default_reschedule(std::shared_ptr<async_node_base> node);

namespace impl
{
/// Tiny per-node spinlock. Critical sections are a few list ops / a single state store — never user code —
/// so spinning stays bounded (a heavier std::mutex per node would cost more for these short holds).
///
/// REVIEW (revisit before this leaves incubator): spinlocks are a deliberate v1 choice, not a settled one.
/// They are fine while critical sections are tiny and uncontended, but they degrade badly under
/// oversubscription / preemption and give no fairness. Reconsider against a proper blocking mutex, a hybrid
/// (spin-then-block), or a lock-free node design once the async system sees real threaded load.
struct async_spinlock
{
    std::atomic_flag _flag; // C++20: default-initialized to the clear state
    void lock()
    {
        while (_flag.test_and_set(std::memory_order_acquire))
            ; // spin until acquired
    }
    void unlock() { _flag.clear(std::memory_order_release); }
};

struct async_spinlock_guard
{
    async_spinlock& _lock;
    explicit async_spinlock_guard(async_spinlock& l) : _lock(l) { _lock.lock(); }
    ~async_spinlock_guard() { _lock.unlock(); }
    async_spinlock_guard(async_spinlock_guard const&) = delete;
    async_spinlock_guard& operator=(async_spinlock_guard const&) = delete;
};
} // namespace impl

// ============================================================================
// async_error — the failure channel, represented as a value (not an exception)
// ============================================================================

/// Distinguishes an ordinary error from a cancellation on the async failure channel.
enum class async_error_kind : cc::u8
{
    error,
    cancelled,
};

/// Value carried on an async's failure channel: either a wrapped cc::any_error or a cancellation.
/// Move-only (follows cc::any_error). A default-constructed async_error is an empty placeholder used for
/// the "no failure yet" slot inside a node; only read it once the node reports has_error().
struct async_error
{
    async_error() = default;

    [[nodiscard]] static async_error make_error(cc::any_error e)
    {
        async_error r;
        r._kind = async_error_kind::error;
        r._error = cc::move(e);
        return r;
    }

    [[nodiscard]] static async_error make_cancelled()
    {
        async_error r;
        r._kind = async_error_kind::cancelled;
        return r;
    }

    [[nodiscard]] bool is_cancelled() const { return _kind == async_error_kind::cancelled; }

    /// The wrapped error; meaningful only when !is_cancelled().
    [[nodiscard]] cc::any_error const& underlying() const { return _error; }
    [[nodiscard]] cc::any_error& underlying() { return _error; }

    async_error(async_error&&) noexcept = default;
    async_error& operator=(async_error&&) noexcept = default;
    async_error(async_error const&) = delete;
    async_error& operator=(async_error const&) = delete;

private:
    async_error_kind _kind = async_error_kind::error;
    cc::any_error _error;
};

// ============================================================================
// result of a single compute step
// ============================================================================

/// What a compute frame reports after one step, once T has been stripped away for the base poll loop.
enum class async_step_status : cc::u8
{
    produced_value, // frame finished; typed value stored in the derived node
    produced_error, // frame finished on the failure channel; error stored in the base node
    waiting,        // frame added dependencies / asked to wait — normalize and poll them now
    yield,          // frame yielded cooperatively — reschedule and come back later
};

/// Status discriminator inside async_result<T> (kept here so async_context can hand out T-agnostic tags).
enum class async_status : cc::u8
{
    value,
    error,
    waiting,
    yield,
};

// tiny tag types returned by async_context helpers; each converts to any async_result<T> (see async.hh)
struct async_waiting_tag
{
};
struct async_yield_tag
{
};
struct async_error_result_tag
{
    async_error error;
};
template <class V>
struct async_success_tag
{
    V value;
};

// ============================================================================
// scheduler seam
// ============================================================================

/// Where runnable nodes go. The async machinery only ever asks a scheduler to make a node runnable; it never
/// owns execution or blocks. A worker binds a scheduler to its thread with async_worker_scope; nodes then
/// reach it via async_scheduler::current(). The default is inline_scheduler (runs on the calling thread); a
/// future work-stealing pool implements the same interface.
///
/// A queued node is passed as a shared_ptr so the scheduler co-owns it while it waits: a node cannot be
/// destroyed while runnable, which is what makes owned children freely schedulable (and steal-safe later).
struct async_scheduler
{
    /// Make a node runnable on the CURRENT worker (local / hot enqueue). Called only when a worker scope is
    /// active on this thread and the node's affinity is compatible with it.
    virtual void enqueue(std::shared_ptr<async_node_base> node) = 0;

    /// Affinity-routed injection: make a node runnable regardless of the calling thread (foreign threads,
    /// cross-affinity wakeups). The default routes to enqueue; a pool overrides this with its injection queue.
    virtual void submit(std::shared_ptr<async_node_base> node) { enqueue(cc::move(node)); }

    virtual ~async_scheduler() = default;

    /// The scheduler bound to the current thread. Asserts if no async_worker_scope is active.
    [[nodiscard]] static async_scheduler& current();
    [[nodiscard]] static async_scheduler* current_or_null();

    /// Affinity served by the worker scope active on this thread (async_affinity::none() if no scope).
    [[nodiscard]] static async_affinity current_affinity();

    /// The process-wide default scheduler that general-compute (bit 0) nodes route to when they cannot run on
    /// the current thread. Null unless one is installed (see install_default_async_pool). Read-mostly:
    /// install once at startup, before the graphs that depend on it run.
    static void set_default(async_scheduler* sched);
    [[nodiscard]] static async_scheduler* default_or_null();
};

/// RAII begin/end of an async worker scope: binds `scheduler` to the calling thread for its lifetime, so
/// node scheduling and polling on this thread route through it. Nesting restores the previous binding.
/// This is the low-level hook that decouples the async graph from any particular executor.
struct async_worker_scope
{
    /// Binds `scheduler` (and the affinity it serves) to the calling thread. `served` defaults to
    /// general-purpose compute (bit 0), matching the inline scheduler and any plain worker.
    explicit async_worker_scope(async_scheduler& scheduler, async_affinity served = async_affinity::general());
    ~async_worker_scope();

    async_worker_scope(async_worker_scope const&) = delete;
    async_worker_scope(async_worker_scope&&) = delete;
    async_worker_scope& operator=(async_worker_scope const&) = delete;
    async_worker_scope& operator=(async_worker_scope&&) = delete;

private:
    async_scheduler* _previous = nullptr;
    async_affinity _previous_affinity;
};

/// The default scheduler: a worker-local LIFO stack pumped on the calling thread. No global lock, no thread
/// ever blocks. LIFO keeps freshly spawned children hot in cache, matching explicit-stack recursion.
struct inline_scheduler final : async_scheduler
{
    void enqueue(std::shared_ptr<async_node_base> node) override { _queue.push_back(cc::move(node)); }

    /// Poll one queued node (LIFO). Returns false if the queue was empty.
    bool run_one();

    /// Pump the queue until `done` returns true or the queue drains.
    void run_until(cc::function_ref<bool()> done);

    [[nodiscard]] bool empty() const { return _queue.empty(); }

private:
    cc::vector<std::shared_ptr<async_node_base>> _queue;
};

// ============================================================================
// async_node_base — untemplated node state + poll loop
// ============================================================================

/// Lifecycle state of a node. Transitions are CAS-based so a dependency completing and scheduling a node can
/// never be lost against that node parking itself (the classic block-vs-wake race).
enum class async_node_state : cc::u8
{
    cold,             // created, never scheduled, compute not started
    scheduled,        // runnable and (logically) queued
    running,          // currently owned by a poller
    blocked,          // parked on not-ready dependencies; continuations installed
    external_pending, // awaiting external completion (a manual/promise node, no compute frame)
    ready,            // terminal: completed with a value or an error
};

/// Shared, T-agnostic node machinery. Holds the atomic state, the pending-dependency list (only not-ready
/// deps, for scheduling), the continuation list (dependents to wake on completion), the owned children
/// (structured lifetime under this frame), and the failure-channel value. The typed value and the compute
/// frame live in the derived typed node (async.hh).
///
/// Concurrency: safe to drive from multiple threads. A per-node spinlock serializes state transitions and
/// continuation/subscription bookkeeping; the state word stays atomic for lock-free is_ready()/is_cold()
/// reads. At most one thread polls a node at a time (try_begin_running), and a completing dependency that
/// wakes a running node sets a re-poll flag instead of enqueuing a second copy. The lock is never held across
/// the user compute frame. Continuations are held as weak_ptrs so a completing dependency can never wake a
/// dependent that is being torn down concurrently.
///
/// Every node is heap-owned through a std::shared_ptr (make_shared): shared_async<T> for public nodes, and a
/// shared_ptr held by the parent for owned once_async children. schedule()/poll() call shared_from_this() and
/// the scheduler co-owns queued nodes through that shared_ptr, so a node MUST be shared-owned before it is
/// ever scheduled or polled — creating one on the stack and driving it is unsupported (shared_from_this()
/// would be undefined). make_async_* / make_once_* / spawn_child all create nodes via make_shared.
///
/// Cacheline-aligned (64 B): nodes are polled and woken concurrently from different threads, so keeping each
/// on its own line avoids false sharing between unrelated nodes.
struct alignas(64) async_node_base : std::enable_shared_from_this<async_node_base>
{
    // queries
public:
    [[nodiscard]] bool is_ready() const { return _state.load(std::memory_order_acquire) == async_node_state::ready; }
    [[nodiscard]] bool has_value() const { return is_ready() && !_is_error; }
    [[nodiscard]] bool has_error() const { return is_ready() && _is_error; }
    [[nodiscard]] bool is_cold() const { return _state.load(std::memory_order_acquire) == async_node_state::cold; }

    /// The failure-channel value; valid only when has_error().
    [[nodiscard]] async_error const& base_error() const { return _error; }

    /// A fresh, independent copy of this node's error for propagation to a dependent. cc::any_error is
    /// move-only and a shared node's error must not be moved out, so the message is re-materialized (the
    /// context chain is not preserved — a richer error-sharing scheme is a follow-up).
    [[nodiscard]] async_error propagate_error() const;

    // debug/introspection (used by tests) — racy on a live node; call only when it is quiescent (single-threaded)
public:
    /// Number of not-ready dependencies currently tracked. Only meaningful between polls.
    [[nodiscard]] cc::isize pending_dependency_count() const { return _pending_deps.size(); }
    /// Number of installed wakeup continuations (may count entries whose dependent has since expired).
    [[nodiscard]] cc::isize continuation_count() const { return _continuations.size(); }
    /// True while the continuation list still fits its inline buffer (no heap dependent-list allocation).
    [[nodiscard]] bool continuations_are_inline() const { return _continuations.is_inline(); }

    // affinity
public:
    [[nodiscard]] async_affinity affinity() const { return _affinity; }

    /// Pin this node to a user-defined task class and give it the route to the pool serving that class. Must be
    /// called before the node is scheduled (asserts otherwise); affinity is frozen once scheduling begins. The
    /// reschedule fn must be non-null and its pool must outlive the node. Bit-0 (general) nodes need no call —
    /// they default to general affinity and the default route.
    void set_affinity(async_affinity a, async_reschedule_fn route);

    // scheduling / driving
public:
    /// Idempotent hint: make this node runnable. Routes to the current worker (if affinity-compatible) or, via
    /// this node's reschedule fn, to the pool serving its affinity. Never implies ownership of execution. Safe
    /// to call from a completed dependency waking many dependents, or twice. A running node records a re-poll
    /// request instead of enqueuing. Requires the node to be shared-owned (created via make_shared).
    void schedule();

    /// Like schedule(), but routes onto `target` specifically (bypassing current-thread/affinity routing).
    /// Used by drivers to place a root on a chosen pool. cold/blocked -> scheduled + target.submit(); a running
    /// node records a re-poll; terminal/already-scheduled nodes are left as-is.
    void schedule_on(async_scheduler& target);

    /// Drive this node forward. Never blocks. Acquires execution ownership (a no-op if another poller owns it
    /// or it is terminal/manual), then loops: drop ready deps, park on the remaining ones (subscribing late),
    /// run one compute step, publish on completion.
    void poll();

    /// Install a one-shot completion callback fired once when this node becomes ready (used by the pool
    /// blocking driver). Returns true if the node was ALREADY ready (no callback installed — do not wait).
    bool install_completion_hook_or_ready(void (*fn)(void*), void* ctx);

    // subscription (called by the poll loop; single-consumer nodes cap the continuation list at 1)
public:
    void add_continuation(async_node_base* dependent);
    void remove_continuation(async_node_base* dependent);

    // ctor / dtor
public:
    async_node_base() = default;

    async_node_base(async_node_base const&) = delete;
    async_node_base(async_node_base&&) = delete;
    async_node_base& operator=(async_node_base const&) = delete;
    async_node_base& operator=(async_node_base&&) = delete;

    virtual ~async_node_base();

    // supplied by the typed node
protected:
    /// Run one step of the compute frame. produced_value/produced_error mean the typed value / base error
    /// have been stored. Not called for external/manual nodes (they have no frame).
    virtual async_step_status poll_compute_step(async_context& ctx) = 0;

    /// Destroy the compute frame (release its captures). Called on completion and teardown, always after the
    /// owned children are destroyed, so a child frame is torn down before the parent frame it borrows from.
    virtual void destroy_frame() = 0;

    // shared helpers for the typed node
protected:
    void set_error(async_error e)
    {
        _is_error = true;
        _error = cc::move(e);
    }

    void set_state(async_node_state s) { _state.store(s, std::memory_order_release); }
    [[nodiscard]] async_node_state state() const { return _state.load(std::memory_order_acquire); }
    void set_single_consumer() { _single_consumer = true; }

    /// Turn this into a push/manual node: awaiting external completion, with the empty affinity mask and no
    /// compute route (it is never run inline; only push_value / push_error complete it).
    void mark_external_pending()
    {
        _affinity = async_affinity::none();
        _reschedule = nullptr;
        set_state(async_node_state::external_pending);
    }

    /// Register `dep` as a not-ready dependency of this node (no subscription yet — that happens late, only
    /// if this node has to park).
    void add_pending_dependency(async_node_base* dep) { _pending_deps.push_back(dep); }

    /// Add an owned child node (structured lifetime under this frame). Held by shared_ptr so the child is
    /// pinned both by this parent and, transitively, by the root shared_async — and can be freely scheduled.
    void adopt_child(std::shared_ptr<async_node_base> child) { _children.push_back(cc::move(child)); }

    /// Mark ready and wake dependents. Used by external/manual completion (no frame/children to tear down).
    void mark_ready_and_notify();

    /// Destroy owned children (running their teardown, which destroys their frames first). The typed node's
    /// destructor calls this before dropping its own frame, enforcing "child frame dies before parent frame".
    void destroy_children();

    /// Remove this node's continuations from every dependency it subscribed to. Must run before a node
    /// destroys its owned children (a child is a dependency it may still be subscribed to).
    void unsubscribe_all();

    // internal
private:
    bool try_begin_running();
    void drop_ready_pending_deps();
    bool subscribe_to_pending_deps();               // returns true if a dep was found already ready (abort parking)
    bool try_subscribe(async_node_base* dependent); // on the dep: subscribe unless already ready
    void route_after_schedule();                    // enqueue exactly once after a cold/blocked -> scheduled transition
    void complete_from_compute();
    void reschedule_self();

    /// Copy affinity + route from a parent (used by spawn_child so a child runs on its parent's pool). Only
    /// valid before the child is scheduled.
    void inherit_affinity_from(async_node_base const& parent)
    {
        _affinity = parent._affinity;
        _reschedule = parent._reschedule;
    }

    // members
private:
    friend struct async_context; // reaches add_pending_dependency / adopt_child / inherit_affinity_from

    impl::async_spinlock _lock; // guards _state transitions, _wake_pending, _continuations (see class doc)

    std::atomic<async_node_state> _state{async_node_state::cold};
    bool _is_error = false;
    bool _single_consumer = false; // once_async: at most one continuation
    bool _wake_pending = false;    // set (under _lock) when a running node is scheduled; makes it re-poll

    async_affinity _affinity = async_affinity::general();        // bit 0 by default
    async_reschedule_fn _reschedule = &async_default_reschedule; // route for the general-compute default

    void (*_on_complete)(void*) = nullptr; // one-shot completion hook (pool blocking driver)
    void* _on_complete_ctx = nullptr;

    async_error _error; // valid iff _is_error

    /// Not-ready dependencies, rebuilt each poll. Only for scheduling/wakeup — it does not own anything.
    /// Poller-private (only the single active poller touches it), so it needs no lock.
    cc::small_vector<async_node_base*, 4> _pending_deps;

    /// Deps we installed a continuation on while parking; used to unsubscribe on wake/teardown. Poller-private.
    cc::small_vector<async_node_base*, 4> _subscribed;

    /// Dependents to reschedule when this node completes, held as weak_ptrs so a wake can never touch a
    /// dependent that is being destroyed concurrently. Inline capacity 1 keeps the once_async single-consumer
    /// case allocation-free; async<T> spills to the heap for many dependents. Guarded by _lock.
    cc::small_vector<std::weak_ptr<async_node_base>, 1> _continuations;

    /// Owned child nodes (shared so a scheduled child stays pinned). Destroyed before the compute frame
    /// (see destroy_children / typed node dtor).
    cc::vector<std::shared_ptr<async_node_base>> _children;
};
} // namespace cc
