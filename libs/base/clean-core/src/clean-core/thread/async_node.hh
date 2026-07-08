#pragma once

#include <clean-core/common/assert.hh>
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
    virtual void enqueue(std::shared_ptr<async_node_base> node) = 0;

    virtual ~async_scheduler() = default;

    /// The scheduler bound to the current thread. Asserts if no async_worker_scope is active.
    [[nodiscard]] static async_scheduler& current();
    [[nodiscard]] static async_scheduler* current_or_null();
};

/// RAII begin/end of an async worker scope: binds `scheduler` to the calling thread for its lifetime, so
/// node scheduling and polling on this thread route through it. Nesting restores the previous binding.
/// This is the low-level hook that decouples the async graph from any particular executor.
struct async_worker_scope
{
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
/// Concurrency: externally synchronized in this first version — the inline scheduler drives one node at a
/// time on one thread. The state word is atomic and transitions are written to be lost-wakeup-free so the
/// same code carries over to a threaded scheduler.
///
/// Every node is heap-owned through a std::shared_ptr (make_shared): shared_async<T> for public nodes, and a
/// shared_ptr held by the parent for owned once_async children. This shared ownership is what pins a node
/// while it sits in a scheduler queue — schedule() enqueues a shared_ptr, so a queued node cannot be
/// destroyed out from under the scheduler. Nodes must therefore be created via make_shared (make_async_* /
/// spawn_child do this); a stack-allocated node must never be scheduled.
struct async_node_base : std::enable_shared_from_this<async_node_base>
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

    // debug/introspection (used by tests)
public:
    /// Number of not-ready dependencies currently tracked. Only meaningful between polls.
    [[nodiscard]] cc::isize pending_dependency_count() const { return _pending_deps.size(); }
    /// Number of installed wakeup continuations.
    [[nodiscard]] cc::isize continuation_count() const { return _continuations.size(); }
    /// True while the continuation list still fits its inline buffer (no heap dependent-list allocation).
    [[nodiscard]] bool continuations_are_inline() const { return _continuations.is_inline(); }

    // scheduling / driving
public:
    /// Idempotent hint: make this node runnable (enqueues a shared_ptr to it, which pins it until polled).
    /// Never implies ownership of execution. Safe to call from a completed dependency waking many dependents,
    /// or twice for the same node. Requires the node to be shared-owned (created via make_shared).
    void schedule();

    /// Drive this node forward. Never blocks. Acquires execution ownership, then loops: drop ready deps, park
    /// on the remaining ones (subscribing late), run one compute step, publish on completion. A no-op if
    /// another poller owns the node or it is already terminal.
    void poll();

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
    void subscribe_to_pending_deps();
    void complete_from_compute();
    void reschedule_self();

    // members
private:
    friend struct async_context; // reaches add_pending_dependency / adopt_child while a frame runs

    std::atomic<async_node_state> _state{async_node_state::cold};
    bool _is_error = false;
    bool _single_consumer = false; // once_async: at most one continuation

    async_error _error; // valid iff _is_error

    /// Not-ready dependencies, rebuilt each poll. Only for scheduling/wakeup — it does not own anything.
    cc::small_vector<async_node_base*, 4> _pending_deps;

    /// Deps we installed a continuation on while parking; used to unsubscribe on wake/teardown.
    cc::small_vector<async_node_base*, 4> _subscribed;

    /// Dependents to reschedule when this node completes. Inline capacity 1 makes the once_async
    /// single-consumer case allocation-free; async<T> spills to the heap for many dependents.
    cc::small_vector<async_node_base*, 1> _continuations;

    /// Owned child nodes (shared so a scheduled child stays pinned). Destroyed before the compute frame
    /// (see destroy_children / typed node dtor).
    cc::vector<std::shared_ptr<async_node_base>> _children;
};
} // namespace cc
