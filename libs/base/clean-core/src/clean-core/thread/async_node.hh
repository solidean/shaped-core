#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/function/function_ref.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/shared_ptr.hh>    // cc::shared_ptr / cc::weak_ptr (intrusive node handles)
#include <clean-core/thread/async_ambient.hh> // the ambient context word each unresolved arm carries
#include <clean-core/thread/atomic.hh>

// Untemplated core of the cc::async dataflow system: the node state machine, the pending-dependency and
// continuation bookkeeping, the scheduler seam, and the failure-channel value type.
// The templated public surface (async<T>, async_context, make_async_*) lives in async.hh.
// The model all of this implements is documented in libs/base/clean-core/docs/systems/async.md.
//
// Nothing here ever blocks a thread.
// poll() drives a node's compute frame forward until it completes, fails as a value, or parks on not-ready
// dependencies with wakeup continuations installed.

// ============================================================================
// async_error — the failure channel, represented as a value (not an exception)
// ============================================================================

/// Distinguishes an ordinary error from a cancellation on the async failure channel.
enum class cc::async_error_kind : cc::u8
{
    error,
    cancelled,
};

/// Value carried on an async's failure channel: either a wrapped cc::any_error or a cancellation.
/// Move-only, following cc::any_error.
/// A default-constructed async_error is an empty placeholder for the "no failure yet" slot inside a node — only read it once the node reports has_error().
struct cc::async_error
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
enum class cc::async_step_status : cc::u8
{
    produced_value, // frame finished; typed value stored in the derived node
    produced_error, // frame finished on the failure channel; error stored in the base node
    waiting,        // frame added dependencies / asked to wait — normalize and poll them now
    yield,          // frame yielded cooperatively — reschedule and come back later
};

namespace cc
{

// ============================================================================
// intrusive node handle: cc::shared_ptr keyed on async_node_base
// ============================================================================

namespace impl
{
/// Refcount traits for the async node: one fused strong/weak count lives inline in async_node_base at offset 0, so a node needs no control block and a handle is one pointer.
/// Keyed on the BASE, so every async<T> shares it via upcast, and a continuation can hold a weak_ptr<async_node_base> cell that actually points at a larger async<T> node.
/// free_storage frees by the concrete size class stashed at construction.
/// destroy_object tears down only the payload — frame, value, error, continuations — and leaves the counts alive, since a weak ref may still read them after the object is gone.
struct async_node_traits
{
    static constexpr bool supports_weak = true;

    // intrusive: the node IS the async node, control included; free by the concrete class stashed in the node
    static constexpr isize node_size(isize payload_size, isize) { return payload_size; }
    static constexpr isize node_align(isize payload_align) { return payload_align; }

    static void init_control(async_node_base* p);
    static void inc_strong(async_node_base* p);
    static cc::shared_release release_strong(async_node_base* p);
    static void inc_weak(async_node_base* p);
    static bool release_weak(async_node_base* p);
    static bool try_lock_strong(async_node_base* p);
    static void destroy_object(async_node_base* p);
    static void free_storage(async_node_base* p);
};
} // namespace impl

/// The owning / weak node handles.
/// shared_async<T> (async.hh) is a cc::shared_ptr<async<T>, async_node_traits> that upcasts to this base handle when handed to the scheduler; continuations are async_node_weak cells.
using async_node_ptr = cc::shared_ptr<async_node_base, impl::async_node_traits>;
using async_node_weak = cc::weak_ptr<async_node_base, impl::async_node_traits>;

// ============================================================================
// scheduler seam
// ============================================================================

} // namespace cc

/// Where runnable nodes go.
/// The async machinery only ever asks a scheduler to make a node runnable — it never owns execution and never blocks.
/// A worker binds a scheduler to its thread with async_worker_scope; nodes reach it via async_scheduler::current().
/// Two implementations ship: singlethreaded_scheduler below, and async_thread_pool (async_thread_pool.hh).
///
/// A queued node is passed as a shared handle, so the scheduler co-owns it while it waits.
/// A node therefore cannot be destroyed while runnable, which is what makes a required dependency freely schedulable and steal-safe.
struct cc::async_scheduler
{
    /// True if a node enqueued here may be picked up by ANOTHER thread.
    /// Fixed at construction, so the poll loop reads it as a plain field rather than paying a virtual call per step.
    /// The poll loop publishes a node's dependencies only when this holds — see "Publish all-but-one" in libs/base/clean-core/docs/systems/async.md.
    bool const has_steal_capable_peers;

    /// Make a node runnable on the CURRENT worker (local / hot enqueue).
    /// Called only when a worker scope is active on this thread.
    virtual void enqueue(async_node_ptr node) = 0;

    /// Injection: make a node runnable regardless of the calling thread — foreign threads, cross-thread wakeups.
    /// The default routes to enqueue; a pool overrides this with its injection queue.
    virtual void submit(async_node_ptr node) { enqueue(cc::move(node)); }

    virtual ~async_scheduler() = default;

protected:
    explicit async_scheduler(bool steal_capable_peers) : has_steal_capable_peers(steal_capable_peers) {}

public:
    /// The scheduler bound to the current thread.
    /// Asserts if no async_worker_scope is active.
    [[nodiscard]] static async_scheduler& current();
    [[nodiscard]] static async_scheduler* current_or_null();

    /// The process-wide default scheduler that compute nodes route to when they cannot run on the current thread.
    /// Null unless one is installed (see install_default_async_pool).
    /// Read-mostly — install once at startup, before the graphs that depend on it run.
    static void set_default(async_scheduler* sched);
    [[nodiscard]] static async_scheduler* default_or_null();
};

/// RAII begin/end of an async worker scope: binds `scheduler` to the calling thread for its lifetime, so node scheduling and polling on this thread route through it.
/// Nesting restores the previous binding.
struct cc::async_worker_scope
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

/// The default scheduler: a LIFO stack pumped on the calling thread, driving everything inline.
/// No global lock, and no thread ever blocks.
///
/// Single-threaded by construction, not by circumstance — it has no peers, so it never publishes work.
/// Progress happens only while this thread is inside blocking_get / run_one / run_until, which is why a graph parked on a manual node needs the pump called again after the external push.
struct cc::singlethreaded_scheduler final : async_scheduler
{
    singlethreaded_scheduler() : async_scheduler(false) {}

    void enqueue(async_node_ptr node) override; // out-of-line: needs the node handle's traits complete

    /// Drive `root` on this thread and return its outcome, or nullopt if this scheduler pumped everything reachable from here and `root` is still not ready.
    /// Nullopt means "not from here, not yet": the graph may be parked on an unpushed manual node, or have migrated onto another scheduler.
    /// Re-driving after the push, or letting the owning scheduler finish, resolves it — see "Multi-scheduler correctness" in libs/base/clean-core/docs/systems/async.md.
    template <class T, class E = async_error>
    [[nodiscard]] cc::optional<cc::result<T, E>> try_blocking_get(shared_async<T, E> const& root);

    /// try_blocking_get, but returns the value directly; asserts on error/cancellation and on no-progress.
    template <class T, class E = async_error>
    [[nodiscard]] T blocking_get(shared_async<T, E> const& root);

    /// Poll one queued node (LIFO). Returns false if the queue was empty.
    bool run_one();

    /// Pump the queue until `done` returns true or the queue drains.
    /// May return with work still queued — that is what `done` means — and the scheduler owns it until drained or destroyed.
    void run_until(cc::function_ref<bool()> done);

    /// Pump until the queue is empty.
    void drain()
    {
        run_until([] { return false; });
    }

    [[nodiscard]] bool empty() const { return _queue.empty(); }

private:
    cc::vector<async_node_ptr> _queue;
};

namespace cc
{

namespace impl
{
/// One entry of a node's not-ready dependency set: a raw, non-owning async_node_base* plus a "subscribed" bit, packed into a single word.
/// A dependency is 64-aligned (async_node_base is alignas(64)), so bits 0..5 are free; bit 1 is the subscribed flag.
/// This proxy edits the packed word in place.
struct async_dep_entry
{
    u64* _word;

    static constexpr u64 subscribed_bit = 0x2;
    static constexpr u64 dep_mask = ~u64(0x3F); // clear the low 6 tag bits to recover the 64-aligned dep

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

/// A spilled-dependency-list node, used only when a node tracks 2+ not-ready deps.
/// node_allocation-backed and intrusively linked; _dep packs the dependency + subscribed bit exactly like a single-mode head.
struct async_dep_list_node
{
    u64 _dep; // 64-aligned async_node_base* in the high bits, subscribed in bit 1
    async_dep_list_node* _next;
};

/// A node's set of not-ready dependencies, folded into a single 8 B tagged word.
/// Only the single active poller ever touches it, so it needs no lock.
/// Move-only; the destructor frees any spilled list nodes.
///
/// _head encoding:
///   0            -> empty
///   bit0 == 0    -> single dep inline: high bits = async_node_base*, bit1 = subscribed
///   bit0 == 1    -> list mode: (_head & ~1) = async_dep_list_node* (first of the chain)
struct async_dep_head
{
    async_dep_head() = default;
    ~async_dep_head()
    {
        if (_head != 0) // empty is the leaf/common case: skip the out-of-line clear entirely
            clear();
    }

    async_dep_head(async_dep_head&& o) noexcept : _head(o._head) { o._head = 0; }
    async_dep_head& operator=(async_dep_head&& o) noexcept
    {
        if (this != &o)
        {
            if (_head != 0)
                clear();
            _head = o._head;
            o._head = 0;
        }
        return *this;
    }
    async_dep_head(async_dep_head const&) = delete;
    async_dep_head& operator=(async_dep_head const&) = delete;

    [[nodiscard]] bool empty() const { return _head == 0; }

    [[nodiscard]] isize count() const
    {
        if (_head == 0)
            return 0;
        if ((_head & tag_is_list) == 0)
            return 1;
        isize n = 0;
        for (auto* p = list_head(); p != nullptr; p = p->_next)
            ++n;
        return n;
    }

    /// The first tracked dependency (any entry), or nullptr if empty.
    /// Used to pick one to drive inline.
    [[nodiscard]] async_node_base* first() const
    {
        if (_head == 0)
            return nullptr;
        u64 const word = (_head & tag_is_list) == 0 ? _head : list_head()->_dep;
        return reinterpret_cast<async_node_base*>(word & async_dep_entry::dep_mask);
    }

    /// Append a not-ready dependency (order irrelevant).
    /// The entry starts unsubscribed.
    void add(async_node_base* dep);
    /// Remove (and free) every entry whose dependency is already ready.
    void remove_ready()
    {
        if (_head != 0) // empty: nothing to scan (the poll loop calls this every turn, incl. on leaves)
            remove_ready_slow();
    }
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
    static constexpr u64 tag_is_list = 0x1;

    [[nodiscard]] async_dep_list_node* list_head() const
    {
        return reinterpret_cast<async_dep_list_node*>(_head & ~tag_is_list);
    }
    void set_list_head(async_dep_list_node* n) { _head = reinterpret_cast<u64>(n) | tag_is_list; }
    void normalize();         // collapse a 0/1-entry list back to empty/single mode
    void remove_ready_slow(); // non-empty scan behind remove_ready's inline empty-guard

    u64 _head = 0;
};

// ============================================================================
// continuation head — dependents to wake on completion (lives in the result slot)
// ============================================================================

/// A spilled continuation entry: either a weak dependent, to schedule, or a one-shot completion latch, to call.
/// node_allocation-backed and intrusively linked, exactly like async_dep_list_node.
/// The union member is left inactive by the default ctor; the allocator constructs the active member.
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

/// A node's set of dependents to wake when it completes (its "continuations"), plus at most a few one-shot completion latches.
/// One tagged word, so it fits the unresolved arm's 24 B budget of ambient 8 + deps 8 + conts 8.
/// Live only BEFORE the node is ready: once ready it is stolen and the payload holds the typed value/error instead, so the two never coexist.
/// Guarded by the node _lock — unlike async_dep_head it has multiple writers, namely other nodes' pollers subscribing and unsubscribing, plus this node completing.
///
/// Encoding mirrors async_dep_head, and EITHER one inline dependent OR a list is live, never both — a 2nd dependent, and every latch, promotes the inline entry into the list first.
///
/// The one difference that matters: its entries are WEAK, where async_dep_head's are non-owning raw pointers.
/// The inline slot has no weak_ptr to do that for it, so it holds exactly one weak count BY HAND — every store inc_weaks, every drop dec_weaks, via weak_ptr::adopt / release.
/// Spill cells keep their own async_node_weak and are self-managing.
struct async_cont_head
{
    async_cont_head() = default;
    ~async_cont_head()
    {
        if (_head != 0) // empty is the leaf/common case: skip the out-of-line clear entirely
            clear();
    }

    async_cont_head(async_cont_head&& o) noexcept : _head(o._head) { o._head = 0; }
    async_cont_head& operator=(async_cont_head&& o) noexcept
    {
        if (this != &o)
        {
            if (_head != 0)
                clear();
            _head = o._head;
            o._head = 0;
        }
        return *this;
    }
    async_cont_head(async_cont_head const&) = delete;
    async_cont_head& operator=(async_cont_head const&) = delete;

    /// True if no dependents and no latches are installed (the leaf case).
    /// Cheap, and safe to test under the lock.
    [[nodiscard]] bool empty() const { return _head == 0; }

    /// Subscribe a dependent (held weakly). Takes the inline slot if free, else prepends a spill cell.
    void add(async_node_base* dependent);
    /// Install a one-shot completion latch (always spills; latches are rare — the pool blocking driver only).
    void add_latch(void (*fn)(void*), void* ctx);
    /// Remove `dependent`, and prune any entries whose dependent has since expired.
    void remove(async_node_base* dependent);
    /// Number of live weak dependents (latches excluded).
    [[nodiscard]] isize count() const;

    /// Fire every entry: schedule each still-live dependent, call each latch.
    /// Call on a stolen (local) head only, never while holding the node lock — scheduling a dependent takes its lock.
    /// Does not consume the entries; the caller's destructor releases them.
    void notify_all();

private:
    static constexpr u64 tag_is_list = 0x1;

    // inline mode (bit0 == 0, _head != 0): the 64-aligned dependent, on which we hold one weak count by hand
    [[nodiscard]] async_node_base* inline_dep() const { return reinterpret_cast<async_node_base*>(_head); }
    [[nodiscard]] async_cont_cell* list_head() const
    {
        return reinterpret_cast<async_cont_cell*>(_head & ~tag_is_list);
    }
    void set_list_head(async_cont_cell* c) { _head = reinterpret_cast<u64>(c) | tag_is_list; }

    void spill_inline(); // move the inline entry into a fresh 1-cell list (2nd dependent, or any latch)
    void release_inline() { auto const w = async_node_weak::adopt(inline_dep()); } // dtor pays our dec_weak
    void normalize(); // collapse an emptied list back to empty
    void clear();     // release the inline ref, or free the whole spill list

    u64 _head = 0;
};

/// The node's transient scratch while it is UNRESOLVED: the ambient context it resumes under, the not-ready dependency set, and the continuation head of dependents to wake.
/// All of it is mutually exclusive with the resolved value/error, which reuses the same storage — so this arm shares the payload slot at offset 16, a union discriminated by state.
/// The value is built straight over this arm at resolution.
/// Manual lifetime: async_node_base placement-constructs this at birth and placement-destroys it when switching to the resolved value/error.
///
/// The COMPUTE FRAME is deliberately not a member: it is the payload TAIL, immediately after this struct, sized by the typed node (async_typed_node<T, E>::frame_capacity).
/// That placement is what keeps `ambient` / `deps` / `conts` at fixed payload offsets 0 / 8 / 16, reachable from the untyped base by constant offset, while the frame slot grows with a large T or E.
struct async_unresolved
{
    /// The ambient context this node resumes under, or null — an opaque head of the async_ambient.hh chain, held STRONGLY.
    /// Written when the node is handed off (queued, parked or yielded) and read when a worker picks it back up; see async.cc.
    /// Never written by a thread that is merely waking the node, which is what keeps one graph's context out of another's.
    void* ambient = nullptr; // payload offset 0
    async_dep_head deps;     // payload offset 8
    async_cont_head conts;   // payload offset 16

    async_unresolved() = default;

    /// Releases the ambient reference; deps + conts free their own list nodes and cells.
    /// The frame is NOT ours — it is a sibling object in the payload, and async_node_base::destroy_frame ends it.
    ///
    /// This is the single release site, and it covers all three ways an arm ends: resolved with a value, resolved with an error, and torn down cold or parked.
    /// Pairing it with the write sites here rather than at those three call sites is what makes the count exactly-once by construction.
    ~async_unresolved() { impl::async_ambient_release(ambient); }

    // Declaring the destructor suppresses the implicit move ctor, which the two heads would otherwise supply.
    // Nothing moves an arm — it is placement-constructed in the payload and stays there — so make that explicit rather than leave it a side effect.
    async_unresolved(async_unresolved&&) = delete;
    async_unresolved& operator=(async_unresolved&&) = delete;
};
static_assert(sizeof(async_cont_head) == 8, "async_cont_head must stay one word — the arm budgets it 8 B");
static_assert(sizeof(async_dep_head) == 8, "async_dep_head must stay one word — the arm budgets it 8 B");

/// Size of the arm, and so the payload offset at which the compute frame starts.
/// Growing it shrinks every node's inline frame slot by the same amount, since the two share one 48 B payload floor.
inline constexpr isize async_arm_bytes = 24;

/// Alignment the inline compute frame can rely on.
/// The payload is 16-aligned and sits at node offset 16, so the frame at payload + 24 lands on absolute offset 40 — 8-aligned, never 16.
/// An over-aligned frame is therefore boxed rather than stored inline (see async_typed_node<T, E>::frame_fits_inline).
inline constexpr isize async_frame_align = 8;

static_assert(sizeof(async_unresolved) == async_arm_bytes,
              "the unresolved arm must stay 24 B: ambient 8 + deps 8 + conts 8");
static_assert(alignof(async_unresolved) <= async_frame_align,
              "the arm must not force padding between itself and the frame tail");
} // namespace impl
} // namespace cc

// ============================================================================
// async_node_base — untemplated node state + poll loop
// ============================================================================

/// Lifecycle state of a node.
/// Transitions are CAS-based, so a dependency completing and scheduling a node can never be lost against that node parking itself.
enum class cc::async_node_state : cc::u8
{
    cold,             // 0  created, never scheduled, compute not started
    scheduled,        // 1  runnable and (logically) queued
    running,          // 2  currently owned by a poller
    blocked,          // 3  parked on not-ready dependencies; continuations installed
    external_pending, // 4  awaiting external completion (a manual/promise node, no compute frame)
    ready_value,      // 5  terminal: completed with a value
    ready_error,      // 6  terminal: completed on the failure channel
    // 7 states -> fits 3 bits (see async_node_base's packed control word).
    // is-error is encoded in the state itself (ready_value vs ready_error), not as a separate flag.
};

/// Type-erased per-async<T, E, F> operations, reached from the untemplated base — the hand-rolled replacement for a C++ vtable.
/// One static-constexpr instance per distinct op set, keyed so it collapses across types; the node points at it from construction, and again once a frame picks F.
/// It recovers what a base-typed pointer cannot: how to destroy the typed value or error, and how to run and destroy the inline frame.
/// It also carries the size class, which the intrusive free path needs long after the concrete type is erased.
///
/// alignas(32) is load-bearing: the node packs the 5 low bits of this pointer with the lifecycle state + wake + lock, so every instance must be 32-aligned to keep those bits free.
struct alignas(32) cc::async_type_ops
{
    void (*teardown_value)(async_node_base*); // destroy the resolved value in the payload (ready_value)
    void (*teardown_error)(async_node_base*); // destroy the resolved error in the payload (ready_error)

    // The compute frame, stored inline in the payload tail just past the unresolved arm.
    // Both null for a frameless node: manual/push, or a born-ready factory.
    // There is deliberately no frame_move — the frame is constructed once in place, run in place, and destroyed in place (see async_node_base's frame section).
    async_step_status (*frame_invoke)(void* frame, async_context_base& ctx);
    void (*frame_destroy)(void* frame);

    /// Fail this node on its error channel from the exception currently being handled — see async_node_base::invoke_frame_step.
    /// Called only from inside a catch handler, and only while the node is still unresolved.
    ///
    /// Null when E declares no exception mapping (cc::custom::async_error_from_exception_trait), which is a runtime diagnostic rather than a compile error.
    /// Keyed on E alone: building the error needs no T, since finish_error_emplace<E> is typed by E only.
    void (*frame_resolve_exception)(async_node_base* node);

    cc::node_class_index class_index; // concrete async<T, E> size class (free_storage frees by it)
};

namespace cc
{
static_assert(alignof(async_type_ops) >= 32, "async_type_ops must be 32-aligned so its low 5 bits are free for tags");

namespace impl
{
/// Type-erased teardown of a resolved payload of type U (value or error), defined in async.hh.
/// Declared here so async_node_base can befriend it — it reaches the protected payload.
/// Keyed on U alone, so the ops descriptor collapses across types (see impl::async_type_ops_for).
template <class U>
void async_typed_teardown(async_node_base* n);

/// Resolve `n` on its failure channel E from the exception being handled, defined in async.hh.
/// Declared here for the same reason: it reaches the protected finish_error_emplace.
template <class E>
void async_frame_resolve_current_exception(async_node_base* n);
} // namespace impl

} // namespace cc

/// Shared, T/E-agnostic node machinery: the atomic state, the not-ready dependency set, the continuation list of dependents to wake, and the type-erased compute frame.
/// The typed value AND the typed error live in the derived typed node (async.hh), sharing payload offset 0 by state.
/// The base builds them via the finish_value* / finish_error* member templates, and reaches their destructors and the size class via async_type_ops.
///
/// Concurrency: safe to drive from multiple threads, and what that guarantees is spelled out under "Multi-scheduler correctness" in libs/base/clean-core/docs/systems/async.md.
/// Two invariants bind the code here: at most one thread polls a node (try_begin_running), and the per-node spinlock is never held across the user compute frame.
/// The state word stays atomic independently of that lock, for lock-free is_ready() / is_cold() reads.
///
/// Every node carries its own intrusive strong/weak refcount (async_node_traits) and is created through cc::make_shared into one slab node.
/// schedule() and poll() recover a handle from `this` in O(1) via async_node_ptr::from_alive, since strong > 0 is guaranteed while polling or scheduling.
/// A node MUST be created via make_async_*: a stack node is unsupported, because from_alive would corrupt a never-initialized count.
///
/// alignas(64) lives on the derived typed node, not here — forcing it on this base alone would round its own size up to 64 and push the typed value and frame onto a second line.
struct cc::async_node_base
{
    // queries
public:
    [[nodiscard]] bool is_ready() const { return is_ready_state(load_state(cc::memory_order_acquire)); }
    [[nodiscard]] bool has_value() const
    {
        return load_state(cc::memory_order_acquire) == async_node_state::ready_value;
    }
    [[nodiscard]] bool has_error() const
    {
        return load_state(cc::memory_order_acquire) == async_node_state::ready_error;
    }
    [[nodiscard]] bool is_cold() const { return load_state(cc::memory_order_acquire) == async_node_state::cold; }

    // The failure-channel value is typed, so it is read and propagated through async<T, E>, not here.
    // The base only knows a node HAS an error (has_error), not its type — see async<T, E>::try_error / propagate_error in async.hh.

    // debug/introspection (used by tests) — racy on a live node; call only when it is quiescent (single-threaded)
public:
    /// Number of not-ready dependencies currently tracked.
    /// Only meaningful between polls, on the unresolved arm.
    [[nodiscard]] isize pending_dependency_count() const { return is_ready() ? 0 : deps().count(); }
    /// Number of installed wakeup continuations (may count entries whose dependent has since expired).
    /// Zero once ready — the continuation head is stolen at completion (the payload then holds value/error).
    [[nodiscard]] isize continuation_count() const { return is_ready() ? 0 : conts().count(); }

    // scheduling / driving
public:
    /// Idempotent hint: make this node runnable.
    /// Routes to the current worker (hot) if a worker scope is active here, else to the installed default pool.
    /// Never implies ownership of execution, and is safe to call twice, or from a completed dependency waking many dependents.
    /// A running node records a re-poll request instead of enqueuing.
    /// The node must be shared-owned, created via make_shared.
    void schedule();

    /// Like schedule(), but routes onto `target` specifically, bypassing current-thread routing.
    /// Used by drivers to place a root on a chosen pool.
    /// cold/blocked -> scheduled + target.submit(); a running node records a re-poll; terminal and already-scheduled nodes are left as-is.
    void schedule_on(async_scheduler& target);

    /// Drive this node forward.
    /// Never blocks.
    /// Acquires execution ownership, a no-op if another poller owns it or it is terminal/manual.
    /// Then loops: drop ready deps, park on the remaining ones subscribing late, run one compute step, publish on completion.
    void poll();

    /// Install a one-shot completion callback, fired once when this node becomes ready (the pool blocking driver uses this).
    /// Returns true if the node was ALREADY ready, in which case no callback was installed and you must not wait.
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

    /// Nodes are never destructed.
    /// They are torn down by teardown_payload at strong 0 and free_storage at weak 0, never by delete — there is no C++ vtable and no virtual dtor.
    /// The implicit, non-virtual destructor is never invoked.
    ~async_node_base() = default;

    // payload — the node's offset-16 slot, raw storage declared by the derived typed node.
    // A hand-managed union of the UNRESOLVED side (the arm, then the compute frame) and the RESOLVED value ⊍ error, discriminated by state.
    // See libs/base/clean-core/docs/systems/async.md for the layout.
    // The base reaches it by pointer arithmetic on `this`, which relies on single inheritance putting the base subobject at offset 0 of the node.
    // The value always overwrites the arm, and reaches the frame's slot too once it is over 24 B; either way it is built only after both have been destroyed — see the frame section below.
    // Manual sub-object lifetime — see finish_value / finish_error / teardown.
protected:
    using frame_type = cc::unique_function<async_step_status(async_context_base&)>;

    static constexpr isize payload_offset = 16; // == sizeof(async_node_base); asserted below the class

    [[nodiscard]] byte* payload() { return reinterpret_cast<byte*>(this) + payload_offset; }
    [[nodiscard]] byte const* payload() const { return reinterpret_cast<byte const*>(this) + payload_offset; }

    // Unresolved arm, active while not ready.
    // Every accessor below reads storage the resolved value overwrites, so NONE of them is valid once the node is ready.
    // That precondition has teeth: `conts` sits at payload offset 16 and `deps` at 8, so an unguarded read after resolution aliases a live value for any T bigger than two words.
    // Callers satisfy it by holding `running`, or by checking the state under the node lock.
    [[nodiscard]] impl::async_unresolved& unresolved() { return *reinterpret_cast<impl::async_unresolved*>(payload()); }
    [[nodiscard]] void*& ambient() { return unresolved().ambient; }
    [[nodiscard]] void* ambient() const { return reinterpret_cast<impl::async_unresolved const*>(payload())->ambient; }
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

    /// The compute frame's storage: the payload TAIL, immediately after the arm.
    /// A constant offset, so reaching the frame needs no ops() indirection — see async_unresolved on why the frame sits last.
    [[nodiscard]] void* frame_storage() { return payload() + impl::async_arm_bytes; }

    // resolved arm, active once ready.
    // The value and the error share payload offset 0, mutually exclusive by state, so both storages alias value_storage().
    // The typed node reinterprets it as T (ready_value) or E (ready_error); the base builds either via the finish_value* / finish_error* member templates below.
    [[nodiscard]] void* value_storage() { return payload(); }

    /// Construct the (empty) unresolved arm into the payload.
    /// Called once from the derived ctor, after set_ops.
    void init_payload() { new (cc::placement_new, payload()) impl::async_unresolved(); }

    // compute frame — its signature is T-agnostic, and it lives INLINE in the payload tail, just past the unresolved arm.
    //
    // The frame is constructed once in place, invoked in place, and destroyed in place, never moved, so parking costs nothing and an immovable frame works.
    // The catch: the resolved value is built from payload offset 0, and a frame resolves RE-ENTRANTLY — `return actx.success(v)` runs finish_value while the closure is still on the stack.
    // So finish_* destroys the live, executing frame before building the value, which is the `delete this;` idiom and carries `delete this;`'s rule —
    //
    //   A FRAME MUST NOT TOUCH ITS CAPTURES AFTER CALLING A resolve_* ACTION.
    //   Resolve is terminal; a tail `return ctx.success(v)` touches nothing afterwards, which is what makes it safe.
    //
    // The resolve arguments themselves are fine: resolve_to_value / resolve_to_error take their value BY VALUE, so what reaches finish_* is a stack temporary.
    // It is never a reference into the captures, or into a dependency the captures pin.
    // The *_emplace forms forward by reference and are the documented exception — see async.hh.
    //
    // Installing a frame needs T/E to pick the ops instance, so the public entry points are async<T, E>'s set_frame / set_frame_emplace; these are the untyped half they build on.
    // The inline budget is type-dependent too, since it is whatever the payload has left past the arm.
    // So the predicate lives on async_typed_node<T, E> rather than here, and reaches install_frame as FrameCapacity.
protected:
    /// Build the frame in place and re-point the node at the ops instance that knows how to run and destroy it — installing the frame is what determines F.
    /// Construction-time only: init_control_word writes the control word with a plain relaxed store, which is safe only before the node is shared.
    template <isize FrameCapacity, class G, class... Args>
    void install_frame(async_type_ops const* ops, Args&&... args)
    {
        static_assert(isize(sizeof(G)) <= FrameCapacity && alignof(G) <= impl::async_frame_align,
                      "frame does not fit the inline slot — set_frame boxes those");
        new (cc::placement_new, frame_storage()) G(cc::forward<Args>(args)...);
        init_control_word(ops, async_node_state::cold);
    }

    /// End the in-place frame's lifetime, if this node has one — manual/push nodes and the born-ready factories are frameless.
    /// Idempotent only in the sense that each teardown path calls it exactly once.
    ///
    /// The frame's bytes are deliberately left as they are: a value of 24 B or less does not reach them, so they simply go stale.
    /// Nothing reads them afterwards, and scrubbing them would be work on the hot completion path buying nothing.
    void destroy_frame()
    {
        if (auto const f = ops()->frame_destroy) // null for a frameless node
            f(frame_storage());
    }

    // completion — steal the continuation head, tear down the unresolved arm, build the result in the payload, wake dependents.
    // finish_value / finish_error are symmetric typed member templates; the emplace forms build in place from raw args, so an immovable T works.
    // Used by the poll loop, resolve_to_value / resolve_to_error, push_value / push_error, and the make_async_from_* factories.
    //
    // Publishes the terminal state LAST (release), then wakes dependents outside the lock.
protected:
    /// Resolve with a value by moving `v` into the payload.
    /// Requires nothrow-move: the move happens under the node lock.
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
        unsubscribe_all(); // the frame is still live and still pins the deps we are unsubscribing from
        destroy_frame();   // the `delete this;` moment: a value over 24 B reaches into the frame's own bytes.
                           // Before the lock, because releasing the captures runs arbitrary user destructors —
                           // a dropped dependency handle can free its node, which must not happen under our spinlock.
        impl::async_cont_head continuations;
        {
            lock_scope g(this);
            if (!conts().empty())                  // leaf/common case has no dependents: skip the steal + wake
                continuations = cc::move(conts()); // steal dependents to wake after we release the lock
            unresolved().~async_unresolved(); // ambient + deps + (now-empty) head, all of which the value overwrites
            new (cc::placement_new, value_storage()) T(cc::forward<Args>(args)...); // value at payload offset 0
            store_state(async_node_state::ready_value);
        }
        if (!continuations.empty())
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
        destroy_frame(); // see finish_value_emplace: same ordering, same reasons
        impl::async_cont_head continuations;
        {
            lock_scope g(this);
            if (!conts().empty())
                continuations = cc::move(conts());
            unresolved().~async_unresolved();
            new (cc::placement_new, value_storage()) E(cc::forward<Args>(args)...); // error at payload offset 0
            store_state(async_node_state::ready_error);
        }
        if (!continuations.empty())
            continuations.notify_all();
    }

    // payload teardown
protected:
    /// Release the active payload arm and the frame, but LEAVE the intrusive counts and _ops alive — a weak ref may still read them after the object is gone.
    /// Called once by async_node_traits at strong 0 (destroy_object); free_storage reclaims the raw node afterwards.
    void teardown_payload();

    // shared helpers for the typed node
protected:
    /// Stash this node's type-erased ops, so the base can destroy the typed value and free the right size class through a base-typed pointer.
    /// Called ONCE from the derived ctor, before the node is shared: it stores the 32-aligned ops pointer into the control word with state=cold.
    /// The ops bits never change afterwards, because free_storage reads them at weak 0 — so teardown_payload never clears them.
    void set_ops(async_type_ops const* ops)
    {
        _state_and_ops.store(reinterpret_cast<u64>(ops), cc::memory_order_relaxed); // state cold, wake/lock clear
    }

    /// Combined ops + initial state store, for construction only: the node is not yet shared, so one plain relaxed store suffices.
    /// It folds set_ops and an initial state transition — the manual/push node births external_pending — that would otherwise not merge across the atomic.
    /// `ops` must be 32-aligned, leaving bits 0..4 free; wake and lock start clear.
    void init_control_word(async_type_ops const* ops, async_node_state state)
    {
        _state_and_ops.store(reinterpret_cast<u64>(ops) | (u64(state) << state_shift), cc::memory_order_relaxed);
    }

    /// Register `dep` as a not-ready dependency of this node.
    /// No subscription yet — that happens late, and only if this node has to park.
    void add_pending_dependency(async_node_base* dep) { deps().add(dep); }

    /// Remove this node's continuations from every dependency it subscribed to.
    /// A no-op with no deps — a leaf, or a node that never parked — since the poll loop and every completion/teardown path call it unconditionally.
    void unsubscribe_all()
    {
        if (!deps().empty())
            unsubscribe_all_slow();
    }

    // internal
private:
    void unsubscribe_all_slow(); // non-empty walk behind unsubscribe_all's inline empty-guard
    bool try_begin_running();
    void drop_ready_pending_deps();
    void schedule_pending_deps(async_node_base* except); // make pending deps runnable; skips `except` if non-null
    bool subscribe_to_pending_deps();               // returns true if a dep was found already ready (abort parking)
    bool try_subscribe(async_node_base* dependent); // on the dep: subscribe unless already ready
    void route_after_schedule();                    // enqueue exactly once after a cold/blocked -> scheduled transition
    void reschedule_self();
    async_step_status invoke_frame_step(async_context_base& ctx); // one compute step, with the frame's exceptions contained

    // packed control word (_state_and_ops) — the low 5 bits tag the 32-aligned ops pointer
private:
    static constexpr u64 lock_bit = 0x1;  // bit 0: the spinlock
    static constexpr u64 wake_bit = 0x2;  // bit 1: re-poll requested for a running node
    static constexpr u64 state_shift = 2; // bits 2..4: async_node_state (7 values)
    static constexpr u64 state_mask = u64(0x7) << state_shift;
    static constexpr u64 ops_mask = ~u64(0x1F); // bits 5..63: the 32-aligned async_type_ops pointer

    static bool is_ready_state(async_node_state s)
    {
        return s == async_node_state::ready_value || s == async_node_state::ready_error;
    }
    [[nodiscard]] async_node_state load_state(cc::memory_order o) const
    {
        return async_node_state((_state_and_ops.load(o) & state_mask) >> state_shift);
    }
    [[nodiscard]] async_type_ops const* ops() const
    {
        return reinterpret_cast<async_type_ops const*>(_state_and_ops.load(cc::memory_order_relaxed) & ops_mask);
    }

    // Lock protocol: acquire the lock bit via a test-and-test-and-set fetch_or, release via fetch_and.
    // While the lock is held only this thread writes the state/wake bits, and readers just acquire-load, so the mutators below are plain load/mask/store.
    // A concurrent spinner's fetch_or only re-sets an already-set lock bit, never changing the value.
    // State stores are release, so a lock-free is_ready() acquire-load that sees a terminal state also sees the value/error published before it.
    //
    // Without threads the lock is uncontendable by construction — the only thread that could hold it is the one asking for it — so both sides compile away.
    // The mutators below are unchanged either way: plain load/mask/store, preserving a bit that simply never gets set.
    void spin_lock()
    {
#if CC_HAS_THREADS
        for (;;)
        {
            if ((_state_and_ops.fetch_or(lock_bit, cc::memory_order_acquire) & lock_bit) == 0)
                return; // set it from clear -> acquired
            while (_state_and_ops.load(cc::memory_order_relaxed) & lock_bit)
                ; // spin-read until the holder releases, then retry the RMW
        }
#endif
    }
    void spin_unlock()
    {
#if CC_HAS_THREADS
        _state_and_ops.fetch_and(~lock_bit, cc::memory_order_release);
#endif
    }

    void store_state(async_node_state s) // under lock (or at construction): set state, preserve ops/lock/wake
    {
        u64 const w = _state_and_ops.load(cc::memory_order_relaxed);
        _state_and_ops.store((w & ~state_mask) | (u64(s) << state_shift), cc::memory_order_release);
    }
    void store_state_clear_wake(async_node_state s) // under lock: set state and clear the wake bit together
    {
        u64 const w = _state_and_ops.load(cc::memory_order_relaxed);
        _state_and_ops.store((w & ~state_mask & ~wake_bit) | (u64(s) << state_shift), cc::memory_order_release);
    }
    void set_wake()
    {
        _state_and_ops.store(_state_and_ops.load(cc::memory_order_relaxed) | wake_bit, cc::memory_order_release);
    }
    void clear_wake()
    {
        _state_and_ops.store(_state_and_ops.load(cc::memory_order_relaxed) & ~wake_bit, cc::memory_order_release);
    }
    [[nodiscard]] bool wake_pending() const { return (_state_and_ops.load(cc::memory_order_relaxed) & wake_bit) != 0; }

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
    friend struct async_context; // typed context reaches finish_value* / finish_error*
    template <class U>
    friend void impl::async_typed_teardown(async_node_base*); // reaches value_storage for the typed dtor
    template <class E>
    friend void impl::async_frame_resolve_current_exception(async_node_base*); // reaches finish_error_emplace
    friend struct impl::async_node_traits; // reaches the intrusive counts / ops / teardown_payload

    /// Intrusive refcount (async_node_traits): strong owners in the high half, weak in the low half — continuation cells plus the strong owners' collective one.
    /// Born 1/1 by init_control.
    /// Fused into one word at offset 0, so the last strong drop can test both counts with a single load and skip both locked RMWs when it is the sole owner (cc::fused_refcount).
    cc::atomic<u64> _counts = {0};

    /// Packed control word: the 32-aligned async_type_ops pointer in bits 5..63, the lifecycle state in bits 2..4, the wake-pending flag in bit 1, the spinlock in bit 0.
    /// Folding lock + state + wake in with the ops pointer is what keeps the fixed header at 16 B alongside _counts.
    /// Set once at construction (set_ops); the ops bits never change, so free_storage can read them at weak 0.
    ///
    /// is_ready() / is_cold() are lock-free acquire loads of this word, so they share an address with the lock RMWs.
    /// That is deliberate — nearly all is_ready() calls target already-resolved nodes, which take no lock at all.
    /// Should a hot pre-completion is_ready() path ever contend, steal the MSB of _counts' weak half for a dedicated ready bit instead.
    cc::atomic<u64> _state_and_ops = {0};

    // No further members: this is a 16 B header.
    // The payload — unresolved scratch ⊍ resolved value/error, including the compute frame — is raw storage declared by the derived async_typed_node<T> at offset 16, via payload().
};

namespace cc
{
static_assert(sizeof(async_node_base) == 16, "async_node_base must be a 16 B header (payload() offset relies on it)");

// ============================================================================
// async_node_traits — intrusive refcount ops (defined now that async_node_base is complete)
// ============================================================================

inline void impl::async_node_traits::init_control(async_node_base* p)
{
    cc::fused_refcount::init(p->_counts);
}
inline void impl::async_node_traits::inc_strong(async_node_base* p)
{
    cc::fused_refcount::inc_strong(p->_counts);
}
inline cc::shared_release impl::async_node_traits::release_strong(async_node_base* p)
{
    return cc::fused_refcount::release_strong(p->_counts);
}
inline void impl::async_node_traits::inc_weak(async_node_base* p)
{
    cc::fused_refcount::inc_weak(p->_counts);
}
inline bool impl::async_node_traits::release_weak(async_node_base* p)
{
    return cc::fused_refcount::release_weak(p->_counts);
}
inline bool impl::async_node_traits::try_lock_strong(async_node_base* p)
{
    return cc::fused_refcount::try_lock_strong(p->_counts);
}
inline void impl::async_node_traits::destroy_object(async_node_base* p)
{
    p->teardown_payload();
}
inline void impl::async_node_traits::free_storage(async_node_base* p)
{
    async_type_ops const* ops = p->ops();
    CC_ASSERT(ops != nullptr, "async node freed without ops (must be created via make_async_* / make_shared)");
    cc::node_allocation_free(reinterpret_cast<byte*>(p), ops->class_index);
}
} // namespace cc
