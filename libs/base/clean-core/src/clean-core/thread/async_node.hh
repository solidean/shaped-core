#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/function/function_ref.hh>
#include <clean-core/fwd.hh>

#include <atomic>
#include <memory> // std::shared_ptr / std::weak_ptr / std::enable_shared_from_this (no cc equivalent yet)

// Untemplated core of the cc::async dataflow system: the node state machine, the pending-dependency /
// continuation bookkeeping, the scheduler seam, and the failure-channel value type. The templated public
// surface (async<T>, async_result<T>, make_async_*, async_blocking_get) lives in async.hh.
//
// Nothing here ever blocks a thread: poll() drives a node's compute frame forward until it completes, fails
// as a value, or parks on not-ready dependencies with wakeup continuations installed. The default driver
// runs everything inline on the calling thread (see inline_scheduler), which is both the shipped default and
// what makes the system testable without threads. A real work-stealing pool is a future layer on the same
// async_scheduler seam.

namespace cc
{
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
    /// active on this thread.
    virtual void enqueue(std::shared_ptr<async_node_base> node) = 0;

    /// Injection: make a node runnable regardless of the calling thread (foreign threads, cross-thread
    /// wakeups). The default routes to enqueue; a pool overrides this with its injection queue.
    virtual void submit(std::shared_ptr<async_node_base> node) { enqueue(cc::move(node)); }

    virtual ~async_scheduler() = default;

    /// The scheduler bound to the current thread. Asserts if no async_worker_scope is active.
    [[nodiscard]] static async_scheduler& current();
    [[nodiscard]] static async_scheduler* current_or_null();

    /// The process-wide default scheduler that compute nodes route to when they cannot run on the current
    /// thread. Null unless one is installed (see install_default_async_pool). Read-mostly: install once at
    /// startup, before the graphs that depend on it run.
    static void set_default(async_scheduler* sched);
    [[nodiscard]] static async_scheduler* default_or_null();
};

/// RAII begin/end of an async worker scope: binds `scheduler` to the calling thread for its lifetime, so
/// node scheduling and polling on this thread route through it. Nesting restores the previous binding.
/// This is the low-level hook that decouples the async graph from any particular executor.
struct async_worker_scope
{
    /// Binds `scheduler` to the calling thread.
    explicit async_worker_scope(async_scheduler& scheduler);
    ~async_worker_scope();

    async_worker_scope(async_worker_scope const&) = delete;
    async_worker_scope(async_worker_scope&&) = delete;
    async_worker_scope& operator=(async_worker_scope const&) = delete;
    async_worker_scope& operator=(async_worker_scope&&) = delete;

private:
    async_scheduler* _previous = nullptr;
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

struct async_node_base; // fwd: dep-head entries hold non-owning base pointers

namespace impl
{
/// One entry of a node's not-ready dependency set: a raw (non-owning) async_node_base* plus a "subscribed"
/// bit, packed into a single word. A dependency is 64-aligned (async_node_base is alignas(64)), so bits 0..5
/// are free; bit 1 is the subscribed flag. This proxy edits the packed word in place.
struct async_dep_entry
{
    cc::u64* _word;

    static constexpr cc::u64 subscribed_bit = 0x2;
    static constexpr cc::u64 dep_mask = ~cc::u64(0x3F); // clear the low 6 tag bits to recover the 64-aligned dep

    [[nodiscard]] async_node_base* dep() const { return reinterpret_cast<async_node_base*>(*_word & dep_mask); }
    [[nodiscard]] bool subscribed() const { return (*_word & subscribed_bit) != 0; }
    void set_subscribed(bool v) const
    {
        if (v)
            *_word |= subscribed_bit;
        else
            *_word &= ~subscribed_bit;
    }
};

/// A spilled-dependency-list node, used only when a node tracks 2+ not-ready deps. node_allocation-backed and
/// intrusively linked; _dep packs the dependency + subscribed bit exactly like a single-mode head.
struct async_dep_list_node
{
    cc::u64 _dep; // 64-aligned async_node_base* in the high bits, subscribed in bit 1
    async_dep_list_node* _next;
};

/// A node's set of not-ready dependencies, folded into a single 8 B tagged word (replaces two 64 B
/// small_vectors). Only the single active poller ever touches it, so it needs no lock. Move-only; the
/// destructor frees any spilled list nodes.
///
/// _head encoding:
///   0            -> empty
///   bit0 == 0    -> single dep inline: high bits = async_node_base*, bit1 = subscribed
///   bit0 == 1    -> list mode: (_head & ~1) = async_dep_list_node* (first of the chain)
struct async_dep_head
{
    async_dep_head() = default;
    ~async_dep_head() { clear(); }

    async_dep_head(async_dep_head&& o) noexcept : _head(o._head) { o._head = 0; }
    async_dep_head& operator=(async_dep_head&& o) noexcept
    {
        if (this != &o)
        {
            clear();
            _head = o._head;
            o._head = 0;
        }
        return *this;
    }
    async_dep_head(async_dep_head const&) = delete;
    async_dep_head& operator=(async_dep_head const&) = delete;

    [[nodiscard]] bool empty() const { return _head == 0; }

    [[nodiscard]] cc::isize count() const
    {
        if (_head == 0)
            return 0;
        if ((_head & tag_is_list) == 0)
            return 1;
        cc::isize n = 0;
        for (auto* p = list_head(); p != nullptr; p = p->_next)
            ++n;
        return n;
    }

    /// Append a not-ready dependency (order irrelevant). The entry starts unsubscribed.
    void add(async_node_base* dep);
    /// Remove (and free) every entry whose dependency is already ready.
    void remove_ready();
    /// Free all list nodes and reset to empty.
    void clear();

    /// Visit every entry in place; f is called as f(async_dep_entry).
    template <class F>
    void for_each(F&& f)
    {
        if (_head == 0)
            return;
        if ((_head & tag_is_list) == 0)
        {
            f(async_dep_entry{&_head});
            return;
        }
        for (auto* p = list_head(); p != nullptr; p = p->_next)
            f(async_dep_entry{&p->_dep});
    }

    /// Visit entries until f returns true; returns true if some f short-circuited, else false.
    template <class F>
    bool for_each_until(F&& f)
    {
        if (_head == 0)
            return false;
        if ((_head & tag_is_list) == 0)
            return f(async_dep_entry{&_head});
        for (auto* p = list_head(); p != nullptr; p = p->_next)
            if (f(async_dep_entry{&p->_dep}))
                return true;
        return false;
    }

private:
    static constexpr cc::u64 tag_is_list = 0x1;

    [[nodiscard]] async_dep_list_node* list_head() const
    {
        return reinterpret_cast<async_dep_list_node*>(_head & ~tag_is_list);
    }
    void set_list_head(async_dep_list_node* n) { _head = reinterpret_cast<cc::u64>(n) | tag_is_list; }
    void normalize(); // collapse a 0/1-entry list back to empty/single mode

    cc::u64 _head = 0;
};
} // namespace impl

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

/// Shared, T-agnostic node machinery. Holds the atomic state, the not-ready dependency set (folded into one
/// packed word, for scheduling/wakeup), the continuation list (dependents to wake on completion), and the
/// failure-channel value. The typed value and the compute frame live in the derived typed node (async.hh).
///
/// Concurrency: safe to drive from multiple threads. A per-node spinlock serializes state transitions and
/// continuation/subscription bookkeeping; the state word stays atomic for lock-free is_ready()/is_cold()
/// reads. At most one thread polls a node at a time (try_begin_running), and a completing dependency that
/// wakes a running node sets a re-poll flag instead of enqueuing a second copy. The lock is never held across
/// the user compute frame. Continuations are held as weak_ptrs so a completing dependency can never wake a
/// dependent that is being torn down concurrently.
///
/// Every node is heap-owned through a std::shared_ptr (make_shared): shared_async<T> for public nodes, and a
/// schedule()/poll() call shared_from_this() and the scheduler co-owns queued nodes through that shared_ptr,
/// so a node MUST be shared-owned before it is ever scheduled or polled — creating one on the stack and
/// driving it is unsupported (shared_from_this() would be undefined). make_async_* create nodes via
/// make_shared.
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
    [[nodiscard]] cc::isize pending_dependency_count() const { return _deps.count(); }
    /// Number of installed wakeup continuations (may count entries whose dependent has since expired).
    [[nodiscard]] cc::isize continuation_count() const { return _continuations.size(); }
    /// True while the continuation list still fits its inline buffer (no heap dependent-list allocation).
    [[nodiscard]] bool continuations_are_inline() const { return _continuations.is_inline(); }

    // scheduling / driving
public:
    /// Idempotent hint: make this node runnable. Routes to the current worker (hot) if a worker scope is active
    /// here, else to the installed default pool. Never implies ownership of execution. Safe to call from a
    /// completed dependency waking many dependents, or twice. A running node records a re-poll request instead
    /// of enqueuing. Requires the node to be shared-owned (created via make_shared).
    void schedule();

    /// Like schedule(), but routes onto `target` specifically (bypassing current-thread routing). Used by
    /// drivers to place a root on a chosen pool. cold/blocked -> scheduled + target.submit(); a running node
    /// records a re-poll; terminal/already-scheduled nodes are left as-is.
    void schedule_on(async_scheduler& target);

    /// Drive this node forward. Never blocks. Acquires execution ownership (a no-op if another poller owns it
    /// or it is terminal/manual), then loops: drop ready deps, park on the remaining ones (subscribing late),
    /// run one compute step, publish on completion.
    void poll();

    /// Install a one-shot completion callback fired once when this node becomes ready (used by the pool
    /// blocking driver). Returns true if the node was ALREADY ready (no callback installed — do not wait).
    bool install_completion_hook_or_ready(void (*fn)(void*), void* ctx);

    // subscription (called by the poll loop)
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

    /// Turn this into a push/manual node: awaiting external completion. It is never run inline (schedule()
    /// bails on external_pending); only push_value / push_error complete it.
    void mark_external_pending() { set_state(async_node_state::external_pending); }

    /// Register `dep` as a not-ready dependency of this node (no subscription yet — that happens late, only
    /// if this node has to park).
    void add_pending_dependency(async_node_base* dep) { _deps.add(dep); }

    /// Mark ready and wake dependents. Used by external/manual completion (no frame to tear down).
    void mark_ready_and_notify();

    /// Remove this node's continuations from every dependency it subscribed to.
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

    // members
private:
    friend struct async_context; // reaches add_pending_dependency on the generic require() path

    impl::async_spinlock _lock; // guards _state transitions, _wake_pending, _continuations (see class doc)

    std::atomic<async_node_state> _state{async_node_state::cold};
    bool _is_error = false;
    bool _wake_pending = false; // set (under _lock) when a running node is scheduled; makes it re-poll

    void (*_on_complete)(void*) = nullptr; // one-shot completion hook (pool blocking driver)
    void* _on_complete_ctx = nullptr;

    async_error _error; // valid iff _is_error

    /// Not-ready dependencies, rebuilt each poll (folded pending + subscribed set). Only for scheduling/wakeup;
    /// it does not own anything (the compute frame's captures keep deps alive). Poller-private (only the single
    /// active poller touches it), so it needs no lock. See impl::async_dep_head for the packed 8 B layout.
    impl::async_dep_head _deps;

    /// Dependents to reschedule when this node completes, held as weak_ptrs so a wake can never touch a
    /// dependent that is being destroyed concurrently. Inline capacity 1 keeps the common single-dependent
    /// case allocation-free; spills to the heap for many dependents. Guarded by _lock.
    cc::small_vector<std::weak_ptr<async_node_base>, 1> _continuations;
};
} // namespace cc
