#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/function/function_ref.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/shared_ptr.hh> // cc::shared_ptr / cc::weak_ptr (intrusive node handles)

#include <atomic>

// Untemplated core of the cc::async dataflow system: the node state machine, the pending-dependency /
// continuation bookkeeping, the scheduler seam, and the failure-channel value type. The templated public
// surface (async<T>, async_context, make_async_*, async_blocking_get) lives in async.hh.
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

// ============================================================================
// intrusive node handle: cc::shared_ptr keyed on async_node_base
// ============================================================================

namespace impl
{
/// Refcount traits for the async node: strong + weak counts live inline in async_node_base (offset 0), so a
/// node needs no separate control block and the handle is one pointer. Keyed on the BASE, so every async<T>
/// shares it via upcast and continuations can hold weak_ptr<async_node_base> cells that actually point at
/// larger async<T> nodes. free_storage frees by the concrete size class stashed at construction; destroy_object
/// tears down only the payload (frame/value/error/continuations) and leaves the counts alive — a weak ref may
/// still read them after the object is gone. Method bodies are defined inline once async_node_base is complete.
struct async_node_traits
{
    static constexpr bool supports_weak = true;

    // intrusive: the node IS the async node, control included; free by the concrete class stashed in the node
    static constexpr cc::isize node_size(cc::isize payload_size, cc::isize) { return payload_size; }
    static constexpr cc::isize node_align(cc::isize payload_align) { return payload_align; }

    static void init_control(async_node_base* p);
    static void inc_strong(async_node_base* p);
    static bool dec_strong(async_node_base* p);
    static void inc_weak(async_node_base* p);
    static bool dec_weak(async_node_base* p);
    static bool try_lock_strong(async_node_base* p);
    static void destroy_object(async_node_base* p);
    static void free_storage(async_node_base* p);
};
} // namespace impl

/// The owning / weak node handles. shared_async<T> (async.hh) is a cc::shared_ptr<async<T>, async_node_traits>
/// that upcasts to this base handle when handed to the scheduler; continuations are async_node_weak cells.
using async_node_ptr = cc::shared_ptr<async_node_base, impl::async_node_traits>;
using async_node_weak = cc::weak_ptr<async_node_base, impl::async_node_traits>;

// ============================================================================
// scheduler seam
// ============================================================================

/// Where runnable nodes go. The async machinery only ever asks a scheduler to make a node runnable; it never
/// owns execution or blocks. A worker binds a scheduler to its thread with async_worker_scope; nodes then
/// reach it via async_scheduler::current(). The default is inline_scheduler (runs on the calling thread); a
/// future work-stealing pool implements the same interface.
///
/// A queued node is passed as a shared handle so the scheduler co-owns it while it waits: a node cannot be
/// destroyed while runnable, which is what makes required dependencies freely schedulable (and steal-safe).
struct async_scheduler
{
    /// Make a node runnable on the CURRENT worker (local / hot enqueue). Called only when a worker scope is
    /// active on this thread.
    virtual void enqueue(async_node_ptr node) = 0;

    /// Injection: make a node runnable regardless of the calling thread (foreign threads, cross-thread
    /// wakeups). The default routes to enqueue; a pool overrides this with its injection queue.
    virtual void submit(async_node_ptr node) { enqueue(cc::move(node)); }

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
    void enqueue(async_node_ptr node) override; // out-of-line: needs the node handle's traits complete

    /// Poll one queued node (LIFO). Returns false if the queue was empty.
    bool run_one();

    /// Pump the queue until `done` returns true or the queue drains.
    void run_until(cc::function_ref<bool()> done);

    [[nodiscard]] bool empty() const { return _queue.empty(); }

private:
    cc::vector<async_node_ptr> _queue;
};

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

    /// The first tracked dependency (any entry), or nullptr if empty. Used to pick one to drive inline.
    [[nodiscard]] async_node_base* first() const
    {
        if (_head == 0)
            return nullptr;
        cc::u64 const word = (_head & tag_is_list) == 0 ? _head : list_head()->_dep;
        return reinterpret_cast<async_node_base*>(word & async_dep_entry::dep_mask);
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

// ============================================================================
// continuation head — dependents to wake on completion (lives in the result slot)
// ============================================================================

/// A spilled continuation entry: either a weak dependent (to schedule) or a one-shot completion latch
/// (to call). node_allocation-backed and intrusively linked, exactly like async_dep_list_node. The
/// union member is left inactive by the default ctor; the allocator constructs the active member.
struct async_cont_cell
{
    async_cont_cell* _next = nullptr;
    void (*_fn)(void*) = nullptr; // null => weak-dependent cell (_weak active); else a latch (_ctx active)
    union
    {
        async_node_weak _weak;
        void* _ctx;
    };

    async_cont_cell() {}  // union left inactive: the allocator initializes _weak or _fn/_ctx
    ~async_cont_cell() {} // the owner destroys _weak (weak cells only) before freeing the slot
    async_cont_cell(async_cont_cell const&) = delete;
    async_cont_cell& operator=(async_cont_cell const&) = delete;
};

/// A node's set of dependents to wake when it completes (its "continuations"), plus at most a few
/// one-shot completion latches. Exactly 16 B, so it fits the unresolved arm's layout (frame 8 + deps 8 +
/// conts 16 = the 32 B scratch). The continuation head is live only BEFORE the node is ready; once ready it
/// is stolen and the payload holds the typed value/error instead — the two never coexist. One weak dependent
/// sits inline — the common single-dependent case pays no allocation; the 2nd+ dependents and every latch
/// spill into a slab-backed intrusive list. Guarded by the node _lock: unlike async_dep_head it has multiple
/// writers (other nodes' pollers subscribing/unsubscribing, plus this node completing).
struct async_cont_head
{
    async_cont_head() = default;
    ~async_cont_head();

    async_cont_head(async_cont_head&& o) noexcept;
    async_cont_head& operator=(async_cont_head&& o) noexcept;
    async_cont_head(async_cont_head const&) = delete;
    async_cont_head& operator=(async_cont_head const&) = delete;

    /// Subscribe a dependent (held weakly). Fills a free inline slot if any, else prepends a spill cell.
    void add(async_node_base* dependent);
    /// Install a one-shot completion latch (always spills; latches are rare — the pool blocking driver only).
    void add_latch(void (*fn)(void*), void* ctx);
    /// Remove `dependent`, and prune any entries whose dependent has since expired.
    void remove(async_node_base* dependent);
    /// Number of live weak dependents (latches excluded).
    [[nodiscard]] cc::isize count() const;

    /// Fire every entry: schedule each still-live dependent, call each latch. Call on a stolen (local)
    /// head only — never while holding the node lock, since scheduling a dependent takes its lock.
    void notify_all();

    // Inline dependent capacity. Sized so the whole head stays 16 B (fits the 32 B unresolved scratch alongside
    // frame + deps); raising it past what fits 16 B grows the scratch and can push the node past one cache line.
    static constexpr cc::isize inline_capacity = 1;

    async_node_weak _inline_deps[inline_capacity]; // a null slot is unused
    async_cont_cell* _spill = nullptr;             // 2nd+ dependents and all latches
};

/// The node's transient scratch while it is UNRESOLVED: the compute frame, the not-ready dependency set, and
/// the continuation head (dependents to wake). All three are mutually exclusive with the resolved value/error,
/// which reuses the same storage — so this arm shares the node's payload slot (offset 16) with the value ⊍
/// error (a union discriminated by state; see async_node_base). The value is built straight over this arm at
/// resolution; the frame is safe because the poll loop moves it onto its own stack for the compute step (and
/// back into the arm if the frame parks). Manual lifetime: async_node_base placement-constructs this at birth
/// and placement-destroys it when switching to the resolved value/error.
struct async_unresolved
{
    cc::unique_function<async_step_status(async_context_base&)> frame; // 8  null for manual/push nodes
    async_dep_head deps;                                               // 8
    async_cont_head conts;                                             // 16
    // Default special members: default ctor births an empty arm; the (non-trivial) default dtor frees frame
    // captures + dep-list nodes + continuation cells. move/copy are implicitly deleted.
};
} // namespace impl

// ============================================================================
// async_node_base — untemplated node state + poll loop
// ============================================================================

/// Lifecycle state of a node. Transitions are CAS-based so a dependency completing and scheduling a node can
/// never be lost against that node parking itself (the classic block-vs-wake race).
enum class async_node_state : cc::u8
{
    cold,             // 0  created, never scheduled, compute not started
    scheduled,        // 1  runnable and (logically) queued
    running,          // 2  currently owned by a poller
    blocked,          // 3  parked on not-ready dependencies; continuations installed
    external_pending, // 4  awaiting external completion (a manual/promise node, no compute frame)
    ready_value,      // 5  terminal: completed with a value
    ready_error,      // 6  terminal: completed on the failure channel
    // 7 states -> fits 3 bits (see async_node_base's packed control word). is-error is encoded in the state
    // itself (ready_value vs ready_error), not a separate flag.
};

/// Type-erased per-async<T, E> operations, reached from the untemplated base — the hand-rolled replacement for
/// a C++ vtable (mirrors unique_function's static descriptor). One static-constexpr instance exists per concrete
/// async type; the node stores a pointer to it, set once at construction. It recovers the things the base cannot
/// derive from a base-typed pointer: how to destroy the typed value or the typed error, and the node's size class
/// (used by the intrusive free path, which runs on a base-typed weak cell after the concrete type is long erased).
/// alignas(32): the node packs the 5 low bits of this pointer with the lifecycle state + wake + lock (see
/// async_node_base's _state_and_ops), so every async_type_ops instance must be 32-aligned to keep those bits
/// free. Objects are static-constexpr globals (one per async type), so the alignment costs nothing meaningful.
struct alignas(32) async_type_ops
{
    void (*teardown_value)(async_node_base*); // destroy the resolved value in the payload (ready_value)
    void (*teardown_error)(async_node_base*); // destroy the resolved error in the payload (ready_error)
    cc::node_class_index class_index;         // concrete async<T, E> size class (free_storage frees by it)
};
static_assert(alignof(async_type_ops) >= 32, "async_type_ops must be 32-aligned so its low 5 bits are free for tags");

/// Shared, T/E-agnostic node machinery. Holds the atomic state, the not-ready dependency set (folded into one
/// packed word, for scheduling/wakeup), the continuation list (dependents to wake on completion), and the
/// type-erased compute frame (its signature carries no T/E). The typed value AND the typed error live in the
/// derived typed node (async.hh, sharing payload offset 0 by state); the base builds them via the finish_value*
/// / finish_error* member templates and reaches their destructors + the size class via async_type_ops.
///
/// Concurrency: safe to drive from multiple threads. A per-node spinlock serializes state transitions and
/// continuation/subscription bookkeeping; the state word stays atomic for lock-free is_ready()/is_cold()
/// reads. At most one thread polls a node at a time (try_begin_running), and a completing dependency that
/// wakes a running node sets a re-poll flag instead of enqueuing a second copy. The lock is never held across
/// the user compute frame. Continuations are held as weak_ptrs so a completing dependency can never wake a
/// dependent that is being torn down concurrently.
///
/// Every node carries its own intrusive strong/weak refcount (async_node_traits) and is created through
/// cc::make_shared into one slab node: shared_async<T> for public nodes. schedule()/poll() recover a handle
/// from `this` in O(1) via async_node_ptr::from_alive (strong > 0 is guaranteed while polling/scheduling), and
/// the scheduler co-owns queued nodes through that handle. A node MUST be created via make_async_* (which
/// make_shared) — a stack node is unsupported (from_alive would corrupt a never-initialized count).
///
/// The concrete node (async<T>) is cacheline-aligned (64 B): nodes are polled and woken concurrently from
/// different threads, so keeping each on its own line avoids false sharing between unrelated nodes. The
/// alignment lives on the derived typed node, not here — this base is the low half of that line, and forcing
/// alignas(64) on it alone would round its own size up to 64 and push the typed value/frame onto a 2nd line.
struct async_node_base
{
    // queries
public:
    [[nodiscard]] bool is_ready() const { return is_ready_state(load_state(std::memory_order_acquire)); }
    [[nodiscard]] bool has_value() const
    {
        return load_state(std::memory_order_acquire) == async_node_state::ready_value;
    }
    [[nodiscard]] bool has_error() const
    {
        return load_state(std::memory_order_acquire) == async_node_state::ready_error;
    }
    [[nodiscard]] bool is_cold() const { return load_state(std::memory_order_acquire) == async_node_state::cold; }

    // The failure-channel value is typed (E), so it is read/propagated through the typed node (async<T, E>),
    // not here — the base only knows a node HAS an error (has_error), not its type. See async<T, E>::try_error /
    // propagate_error in async.hh.

    // debug/introspection (used by tests) — racy on a live node; call only when it is quiescent (single-threaded)
public:
    /// Number of not-ready dependencies currently tracked. Only meaningful between polls (unresolved arm).
    [[nodiscard]] cc::isize pending_dependency_count() const { return is_ready() ? 0 : deps().count(); }
    /// Number of installed wakeup continuations (may count entries whose dependent has since expired).
    /// Zero once ready — the continuation head is stolen at completion (the payload then holds value/error).
    [[nodiscard]] cc::isize continuation_count() const { return is_ready() ? 0 : conts().count(); }

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

    /// Nodes are never destructed: they are torn down by teardown_payload (at strong 0) + free_storage (at
    /// weak 0), never by delete — there is no C++ vtable and no virtual dtor. The implicit (non-virtual)
    /// destructor is never invoked.
    ~async_node_base() = default;

    // payload — the node's offset-16 slot (raw storage declared by the derived typed node): a hand-managed
    // union of the UNRESOLVED scratch (frame + deps + conts) and the RESOLVED value ⊍ error, discriminated by
    // state. The resolved value/error is built straight over the scratch, so it grows the node naturally for a
    // large T (no inline cap). The base reaches the payload by pointer arithmetic on `this` (single inheritance,
    // base-first: the base subobject is at offset 0 of the node). Building the value over the frame's slot is
    // safe because the poll loop moves the frame onto its own stack for the compute step (and back into the arm
    // if the frame parks). Manual sub-object lifetime — see finish_value / finish_error / teardown.
protected:
    using frame_type = cc::unique_function<async_step_status(async_context_base&)>;

    static constexpr cc::isize payload_offset = 16; // == sizeof(async_node_base); asserted below the class

    [[nodiscard]] cc::byte* payload() { return reinterpret_cast<cc::byte*>(this) + payload_offset; }
    [[nodiscard]] cc::byte const* payload() const { return reinterpret_cast<cc::byte const*>(this) + payload_offset; }

    // unresolved arm (active while not ready)
    [[nodiscard]] impl::async_unresolved& unresolved() { return *reinterpret_cast<impl::async_unresolved*>(payload()); }
    [[nodiscard]] frame_type& frame() { return unresolved().frame; }
    [[nodiscard]] impl::async_dep_head& deps() { return unresolved().deps; }
    [[nodiscard]] impl::async_dep_head const& deps() const
    {
        return reinterpret_cast<impl::async_unresolved const*>(payload())->deps;
    }
    [[nodiscard]] impl::async_cont_head& conts() { return unresolved().conts; }
    [[nodiscard]] impl::async_cont_head const& conts() const
    {
        return reinterpret_cast<impl::async_unresolved const*>(payload())->conts;
    }

    // resolved arm (active once ready). The value and the error share payload offset 0 (mutually exclusive by
    // state), so both storages alias value_storage(); the typed node reinterprets it as T (ready_value) or E
    // (ready_error). The base builds either via the finish_value*/finish_error* member templates below.
    [[nodiscard]] void* value_storage() { return payload(); }

    /// Construct the (empty) unresolved arm into the payload. Called once from the derived ctor (after set_ops).
    void init_payload() { new (cc::placement_new, payload()) impl::async_unresolved(); }

    // compute frame — its signature is T-agnostic; it lives in the unresolved arm of the payload
public:
    /// Install the compute frame — `async_step_status(async_context_base&)`; it resolves its outcome via the
    /// context (a typed async_context<T, E> the frame closure wraps around the base for resolve/emplace).
    template <class F>
    void set_frame(F&& f)
    {
        frame() = frame_type(cc::forward<F>(f));
    }

    // completion — steal the continuation head, tear down the unresolved arm, build the result in the payload,
    // wake dependents. finish_value / finish_error are symmetric typed member templates (the (typed) construction
    // lives here); the emplace forms build in place from raw args so an immovable T works. Used by the poll loop,
    // resolve_to_value / resolve_to_error, push_value / push_error, and the make_async_from_* factories.
    //
    // On the compute path the frame is already moved onto the poll stack, so the frame slot the result overwrites
    // holds only a moved-from shell (the live closure is on the stack); on the push/factory path the frame slot
    // is empty. Publishes the terminal state LAST (release), then wakes dependents outside the lock.
protected:
    /// Resolve with a value by moving `v` into the payload. Requires nothrow-move (moved under the node lock).
    template <class T>
    void finish_value(T&& v)
    {
        static_assert(std::is_nothrow_move_constructible_v<std::decay_t<T>>,
                      "finish_value moves the value under the node lock — it must be nothrow-move-constructible; "
                      "use resolve_to_value_emplace / make_async_from_value_emplace for a non-movable value");
        finish_value_emplace<std::decay_t<T>>(cc::forward<T>(v));
    }

    /// Resolve with a value built in place from `args` (never moved) — the immovable-T path.
    template <class T, class... Args>
    void finish_value_emplace(Args&&... args)
    {
        unsubscribe_all(); // the frame still pins the deps we are unsubscribing from
        impl::async_cont_head continuations;
        {
            lock_scope g(this);
            continuations = cc::move(conts());
            unresolved().~async_unresolved(); // frame shell + deps + moved-from head
            new (cc::placement_new, value_storage()) T(cc::forward<Args>(args)...); // value at payload offset 0
            store_state(async_node_state::ready_value);
        }
        continuations.notify_all(); // outside the lock: waking a dependent / firing a latch takes other locks
    }

    /// Resolve on the failure channel by moving `e` into the payload (the typed twin of finish_value).
    template <class E>
    void finish_error(E&& e)
    {
        finish_error_emplace<std::decay_t<E>>(cc::forward<E>(e));
    }

    /// Resolve on the failure channel with an error built in place from `args`.
    template <class E, class... Args>
    void finish_error_emplace(Args&&... args)
    {
        unsubscribe_all();
        impl::async_cont_head continuations;
        {
            lock_scope g(this);
            continuations = cc::move(conts());
            unresolved().~async_unresolved();
            new (cc::placement_new, value_storage()) E(cc::forward<Args>(args)...); // error at payload offset 0
            store_state(async_node_state::ready_error);
        }
        continuations.notify_all();
    }

    // payload teardown
protected:
    /// Release the active payload arm + the frame but LEAVE the intrusive counts (and _ops) alive — a weak ref
    /// may still read them after the object is gone. Called once by async_node_traits at strong 0
    /// (destroy_object); free_storage reclaims the raw node afterward.
    void teardown_payload();

    // shared helpers for the typed node
protected:
    /// Stash this node's type-erased ops (its static async_type_ops), so the base can destroy the typed value
    /// and free the right size class through a base-typed pointer. Called ONCE from the derived ctor, before
    /// the node is shared — stores the 32-aligned ops pointer into the control word with state=cold. The ops
    /// bits never change afterwards (free_storage reads them at weak 0), so teardown_payload never clears them.
    void set_ops(async_type_ops const* ops)
    {
        _state_and_ops.store(reinterpret_cast<cc::u64>(ops), std::memory_order_relaxed); // state cold, wake/lock clear
    }

    /// Turn this into a push/manual node: awaiting external completion. It is never run inline (schedule()
    /// bails on external_pending); only push_value / push_error complete it. Construction-time (set_manual),
    /// before the node is shared, so no lock is needed.
    void mark_external_pending() { store_state(async_node_state::external_pending); }

    /// Register `dep` as a not-ready dependency of this node (no subscription yet — that happens late, only
    /// if this node has to park).
    void add_pending_dependency(async_node_base* dep) { deps().add(dep); }

    /// Remove this node's continuations from every dependency it subscribed to.
    void unsubscribe_all();

    // internal
private:
    bool try_begin_running();
    void drop_ready_pending_deps();
    bool subscribe_to_pending_deps();               // returns true if a dep was found already ready (abort parking)
    bool try_subscribe(async_node_base* dependent); // on the dep: subscribe unless already ready
    void route_after_schedule();                    // enqueue exactly once after a cold/blocked -> scheduled transition
    void reschedule_self();

    // packed control word (_state_and_ops) — the low 5 bits tag the 32-aligned ops pointer
private:
    static constexpr cc::u64 lock_bit = 0x1;  // bit 0: the spinlock
    static constexpr cc::u64 wake_bit = 0x2;  // bit 1: re-poll requested for a running node
    static constexpr cc::u64 state_shift = 2; // bits 2..4: async_node_state (7 values)
    static constexpr cc::u64 state_mask = cc::u64(0x7) << state_shift;
    static constexpr cc::u64 ops_mask = ~cc::u64(0x1F); // bits 5..63: the 32-aligned async_type_ops pointer

    static bool is_ready_state(async_node_state s)
    {
        return s == async_node_state::ready_value || s == async_node_state::ready_error;
    }
    [[nodiscard]] async_node_state load_state(std::memory_order o) const
    {
        return async_node_state((_state_and_ops.load(o) & state_mask) >> state_shift);
    }
    [[nodiscard]] async_type_ops const* ops() const
    {
        return reinterpret_cast<async_type_ops const*>(_state_and_ops.load(std::memory_order_relaxed) & ops_mask);
    }

    // Lock protocol: acquire the lock bit via a test-and-test-and-set fetch_or, release via fetch_and. While
    // the lock is held only this thread writes the state/wake bits (readers just acquire-load), so the mutators
    // below are plain load/mask/store — a concurrent spinner's fetch_or only re-sets an already-set lock bit,
    // never changing the value. State stores are release, so a lock-free is_ready() acquire-load that sees a
    // terminal state also sees the value/error published before it.
    void spin_lock()
    {
        for (;;)
        {
            if ((_state_and_ops.fetch_or(lock_bit, std::memory_order_acquire) & lock_bit) == 0)
                return; // set it from clear -> acquired
            while (_state_and_ops.load(std::memory_order_relaxed) & lock_bit)
                ; // spin-read until the holder releases, then retry the RMW
        }
    }
    void spin_unlock() { _state_and_ops.fetch_and(~lock_bit, std::memory_order_release); }

    void store_state(async_node_state s) // under lock (or at construction): set state, preserve ops/lock/wake
    {
        cc::u64 const w = _state_and_ops.load(std::memory_order_relaxed);
        _state_and_ops.store((w & ~state_mask) | (cc::u64(s) << state_shift), std::memory_order_release);
    }
    void store_state_clear_wake(async_node_state s) // under lock: set state and clear the wake bit together
    {
        cc::u64 const w = _state_and_ops.load(std::memory_order_relaxed);
        _state_and_ops.store((w & ~state_mask & ~wake_bit) | (cc::u64(s) << state_shift), std::memory_order_release);
    }
    void set_wake()
    {
        _state_and_ops.store(_state_and_ops.load(std::memory_order_relaxed) | wake_bit, std::memory_order_release);
    }
    void clear_wake()
    {
        _state_and_ops.store(_state_and_ops.load(std::memory_order_relaxed) & ~wake_bit, std::memory_order_release);
    }
    [[nodiscard]] bool wake_pending() const { return (_state_and_ops.load(std::memory_order_relaxed) & wake_bit) != 0; }

    struct lock_scope
    {
        async_node_base* n;
        explicit lock_scope(async_node_base* node) : n(node) { n->spin_lock(); }
        ~lock_scope() { n->spin_unlock(); }
        lock_scope(lock_scope const&) = delete;
        lock_scope& operator=(lock_scope const&) = delete;
    };

    // members
private:
    friend struct async_context_base; // reaches add_pending_dependency on the generic require() path
    template <class, class>
    friend struct async_context;           // typed context reaches finish_value* / finish_error*
    friend struct impl::async_node_traits; // reaches the intrusive counts / ops / teardown_payload

    /// Intrusive refcount (async_node_traits): strong owners + weak (continuation cells + the strong owners'
    /// collective one). Born 1/1 by init_control. Kept as two independent atomics (offset 0) so inc/dec stay
    /// plain fetch_add — the state/lock live in a separate word.
    std::atomic<cc::u32> _strong{0};
    std::atomic<cc::u32> _weak{0};

    /// Packed control word: the 32-aligned async_type_ops pointer in bits 5..63, the lifecycle state in bits
    /// 2..4, the wake-pending flag in bit 1, and the spinlock in bit 0. Folding lock + state + wake in with the
    /// ops pointer keeps the fixed header at 16 B (with _strong/_weak). Set once at construction (set_ops); the
    /// ops bits never change, so free_storage can read them at weak 0.
    ///
    /// NOTE: is_ready()/is_cold() are lock-free acquire loads of this word, so they share an address with the
    /// lock RMWs. Deliberate: nearly all is_ready() calls target already-resolved nodes, which take no lock
    /// (completion is done) — no contention there. If a hot pre-completion is_ready() path ever contends,
    /// steal the MSB of _weak for a dedicated ready bit instead.
    std::atomic<cc::u64> _state_and_ops{0};

    // No further members: this is a 16 B header. The payload (unresolved scratch ⊍ resolved value/error, incl.
    // the compute frame) is raw storage declared by the derived async_typed_node<T> at offset 16, via payload().
};
static_assert(sizeof(async_node_base) == 16, "async_node_base must be a 16 B header (payload() offset relies on it)");

// ============================================================================
// async_node_traits — intrusive refcount ops (defined now that async_node_base is complete)
// ============================================================================

inline void impl::async_node_traits::init_control(async_node_base* p)
{
    p->_strong.store(1, std::memory_order_relaxed);
    p->_weak.store(1, std::memory_order_relaxed);
}
inline void impl::async_node_traits::inc_strong(async_node_base* p)
{
    p->_strong.fetch_add(1, std::memory_order_relaxed);
}
inline bool impl::async_node_traits::dec_strong(async_node_base* p)
{
    return p->_strong.fetch_sub(1, std::memory_order_acq_rel) == 1;
}
inline void impl::async_node_traits::inc_weak(async_node_base* p)
{
    p->_weak.fetch_add(1, std::memory_order_relaxed);
}
inline bool impl::async_node_traits::dec_weak(async_node_base* p)
{
    return p->_weak.fetch_sub(1, std::memory_order_acq_rel) == 1;
}
inline bool impl::async_node_traits::try_lock_strong(async_node_base* p)
{
    cc::u32 cur = p->_strong.load(std::memory_order_relaxed);
    while (cur != 0)
        if (p->_strong.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            return true;
    return false; // lost the race to the last strong drop -> the node is (being) torn down
}
inline void impl::async_node_traits::destroy_object(async_node_base* p)
{
    p->teardown_payload();
}
inline void impl::async_node_traits::free_storage(async_node_base* p)
{
    async_type_ops const* ops = p->ops();
    CC_ASSERT(ops != nullptr, "async node freed without ops (must be created via make_async_* / make_shared)");
    cc::node_allocation_free(reinterpret_cast<cc::byte*>(p), ops->class_index);
}
} // namespace cc
