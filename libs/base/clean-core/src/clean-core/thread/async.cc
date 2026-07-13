#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>

// Untemplated core of the async runtime: the per-thread scheduler binding, the inline scheduler pump, and
// the node state machine / poll loop. See async_node.hh for the shape and invariants.
//
// Concurrency model (safe to drive from many threads):
//   * a per-node spinlock (_lock) serializes state transitions and continuation/subscription bookkeeping;
//   * the state word stays atomic for lock-free is_ready()/is_cold() reads;
//   * at most one thread polls a node (try_begin_running); a completing dependency that wakes a running node
//     sets _wake_pending instead of enqueuing a second copy, and the active poller re-polls;
//   * the lock is never held across the user compute frame;
//   * continuations are weak_ptrs, so a wake can never touch a dependent torn down concurrently.

namespace
{
// The scheduler bound to the calling thread (set by async_worker_scope). thread_local even without threads: a
// single-threaded build just has one slot. nullptr => no worker scope active.
thread_local cc::async_scheduler* s_current_scheduler = nullptr;

// Process-wide default scheduler for compute nodes that cannot run on the current thread. Read-mostly:
// installed once at startup. Atomic so installation is visible to worker threads without extra synchronization.
std::atomic<cc::async_scheduler*> s_default_scheduler{nullptr};

// spilled dependency-list nodes come from the node slab allocator (wait-free free, cross-thread safe): a node
// parked by one worker may be re-polled/torn down by another, which then frees these on a different thread.
cc::impl::async_dep_list_node* dep_alloc_node(cc::u64 dep_word)
{
    constexpr auto idx = cc::node_class_index_for<cc::impl::async_dep_list_node>();
    auto* raw = cc::default_node_allocator().allocate_node_bytes(idx, sizeof(cc::impl::async_dep_list_node),
                                                                 alignof(cc::impl::async_dep_list_node));
    return new (cc::placement_new, raw) cc::impl::async_dep_list_node{dep_word, nullptr};
}

void dep_free_node(cc::impl::async_dep_list_node* n)
{
    // async_dep_list_node is trivially destructible (u64 + raw ptr), so no explicit dtor call is needed
    cc::node_allocation_free(reinterpret_cast<cc::byte*>(n), cc::node_class_index_for<cc::impl::async_dep_list_node>());
}

// spilled continuation cells share the same wait-free node slab (a node parked by one worker may be
// re-polled / torn down by another, freeing these on a different thread).
cc::impl::async_cont_cell* cont_alloc_cell()
{
    constexpr auto idx = cc::node_class_index_for<cc::impl::async_cont_cell>();
    auto* raw = cc::default_node_allocator().allocate_node_bytes(idx, sizeof(cc::impl::async_cont_cell),
                                                                 alignof(cc::impl::async_cont_cell));
    return new (cc::placement_new, raw) cc::impl::async_cont_cell();
}

void cont_free_cell(cc::impl::async_cont_cell* c)
{
    if (c->_fn == nullptr) // weak-dependent cell: end the weak's lifetime (dec_weak, maybe free that node)
    {
        using weak_t = cc::async_node_weak;
        c->_weak.~weak_t();
    }
    c->~async_cont_cell();
    cc::node_allocation_free(reinterpret_cast<cc::byte*>(c), cc::node_class_index_for<cc::impl::async_cont_cell>());
}

// Per-worker recursion depth of the eager depth-first dep drive (poll() calling a dependency's poll()). Caps
// the native stack at graph-depth; past the cap we fall back to subscribe+park, which uses no extra stack.
thread_local int s_inline_depth = 0;
constexpr int async_max_inline_depth = 128;

struct inline_depth_guard
{
    inline_depth_guard() { ++s_inline_depth; }
    ~inline_depth_guard() { --s_inline_depth; }
    inline_depth_guard(inline_depth_guard const&) = delete;
    inline_depth_guard& operator=(inline_depth_guard const&) = delete;
};
} // namespace

// ============================================================================
// scheduler seam
// ============================================================================

cc::async_scheduler& cc::async_scheduler::current()
{
    CC_ASSERT(s_current_scheduler != nullptr, "no async worker scope active on this thread");
    return *s_current_scheduler;
}

cc::async_scheduler* cc::async_scheduler::current_or_null()
{
    return s_current_scheduler;
}

void cc::async_scheduler::set_default(async_scheduler* sched)
{
    s_default_scheduler.store(sched, std::memory_order_release);
}

cc::async_scheduler* cc::async_scheduler::default_or_null()
{
    return s_default_scheduler.load(std::memory_order_acquire);
}

cc::async_worker_scope::async_worker_scope(cc::async_scheduler& scheduler) : _previous(s_current_scheduler)
{
    s_current_scheduler = &scheduler;
}

cc::async_worker_scope::~async_worker_scope()
{
    s_current_scheduler = _previous;
}

void cc::inline_scheduler::enqueue(async_node_ptr node)
{
    _queue.push_back(cc::move(node));
}

bool cc::inline_scheduler::run_one()
{
    if (_queue.empty())
        return false;

    auto node = cc::move(_queue.back()); // keep it alive across the poll, then release our queue ref
    _queue.remove_back();
    node->poll();
    return true;
}

void cc::inline_scheduler::run_until(cc::function_ref<bool()> done)
{
    while (!done() && run_one())
    {
    }
}

// ============================================================================
// async_node_base — state transitions
// ============================================================================

void cc::async_node_base::schedule()
{
    {
        impl::async_spinlock_guard g(_lock);
        auto const s = _state.load(std::memory_order_relaxed);

        // terminal, already runnable, or a manual node (only external completion makes those ready)
        if (s == async_node_state::ready || s == async_node_state::scheduled || s == async_node_state::external_pending)
            return;

        if (s == async_node_state::running)
        {
            // a second poller must never run this node: record a re-poll request; the active poller reconciles
            // at its next park point instead of parking.
            _wake_pending = true;
            return;
        }

        // cold or blocked -> make runnable (we route exactly once, below, after releasing the lock)
        _state.store(async_node_state::scheduled, std::memory_order_release);
    }

    route_after_schedule();
}

void cc::async_node_base::schedule_on(async_scheduler& target)
{
    bool do_submit = false;
    {
        impl::async_spinlock_guard g(_lock);
        auto const s = _state.load(std::memory_order_relaxed);

        if (s == async_node_state::ready || s == async_node_state::scheduled || s == async_node_state::external_pending)
            return; // terminal, already runnable elsewhere, or a manual node

        if (s == async_node_state::running)
        {
            _wake_pending = true;
            return;
        }

        _state.store(async_node_state::scheduled, std::memory_order_release);
        do_submit = true;
    }

    if (do_submit)
        target.submit(async_node_ptr::from_alive(this)); // strong > 0: our caller holds a handle
}

void cc::async_node_base::route_after_schedule()
{
    // State is `scheduled` and nobody else will enqueue it (schedule() is idempotent on `scheduled`), so we
    // route exactly once: the current worker (hot) if a scope is active here, else the installed default pool.
    // The default-pool fallback is thread-independent, which is what makes cross-thread wakeups correct.
    auto self = async_node_ptr::from_alive(this); // strong > 0 throughout scheduling (our caller holds a handle)
    if (auto* sched = async_scheduler::current_or_null())
    {
        sched->enqueue(cc::move(self));
        return;
    }

    if (auto* d = async_scheduler::default_or_null())
    {
        d->submit(cc::move(self));
        return;
    }

    CC_ASSERT(false, "no scheduler to route a compute async: install a default async pool or drive it inside an "
                     "async_worker_scope");
}

bool cc::async_node_base::try_begin_running()
{
    impl::async_spinlock_guard g(_lock);
    auto const s = _state.load(std::memory_order_relaxed);

    // another poller owns it, it is terminal, or it awaits external completion -> not runnable here
    if (s == async_node_state::ready || s == async_node_state::running || s == async_node_state::external_pending)
        return false;

    _state.store(async_node_state::running, std::memory_order_release);
    _wake_pending = false; // start fresh; any wake during this run re-sets it
    return true;
}

void cc::async_node_base::reschedule_self()
{
    // yield: the one legitimate self-driven running -> scheduled transition. Bypasses the wake-suppression in
    // schedule() (which would leave a running node un-enqueued). A yield stays on the current, compatible
    // worker, so route_after_schedule takes the local hot path.
    {
        impl::async_spinlock_guard g(_lock);
        CC_ASSERT(_state.load(std::memory_order_relaxed) == async_node_state::running, "yield from a non-running node");
        _state.store(async_node_state::scheduled, std::memory_order_release);
        _wake_pending = false;
    }
    route_after_schedule();
}

// ============================================================================
// async_dep_head — packed not-ready dependency set (see async_node.hh)
// ============================================================================

void cc::impl::async_dep_head::add(async_node_base* dep)
{
    auto const dv = reinterpret_cast<cc::u64>(dep); // 64-aligned: low 6 bits clear, so unsubscribed

    if (_head == 0) // empty -> single (bit0 == 0 => not a list)
    {
        _head = dv;
        return;
    }

    if ((_head & tag_is_list) == 0) // single -> start a list carrying the existing entry
        set_list_head(dep_alloc_node(_head));

    auto* n = dep_alloc_node(dv); // prepend the new dep
    n->_next = list_head();
    set_list_head(n);
}

void cc::impl::async_dep_head::remove_ready()
{
    if (_head == 0)
        return;

    if ((_head & tag_is_list) == 0)
    {
        auto* dep = reinterpret_cast<async_node_base*>(_head & async_dep_entry::dep_mask);
        if (dep->is_ready())
            _head = 0;
        return;
    }

    async_dep_list_node* prev = nullptr;
    for (auto* n = list_head(); n != nullptr;)
    {
        auto* const next = n->_next;
        auto* dep = reinterpret_cast<async_node_base*>(n->_dep & async_dep_entry::dep_mask);
        if (dep->is_ready())
        {
            if (prev != nullptr)
                prev->_next = next;
            else
                set_list_head(next);
            dep_free_node(n);
        }
        else
            prev = n;
        n = next;
    }
    normalize();
}

void cc::impl::async_dep_head::normalize()
{
    if ((_head & tag_is_list) == 0)
        return; // empty or single already

    auto* h = list_head();
    if (h == nullptr)
        _head = 0;                // list emptied
    else if (h->_next == nullptr) // exactly one left -> collapse to single (carries its subscribed bit)
    {
        _head = h->_dep;
        dep_free_node(h);
    }
}

void cc::impl::async_dep_head::clear()
{
    if ((_head & tag_is_list) != 0)
        for (auto* n = list_head(); n != nullptr;)
        {
            auto* const next = n->_next;
            dep_free_node(n);
            n = next;
        }
    _head = 0;
}

// ============================================================================
// async_cont_head — continuation set (up to 3 inline weak dependents + a spill list; see async_node.hh)
// ============================================================================

cc::impl::async_cont_head::~async_cont_head()
{
    for (auto* c = _spill; c != nullptr;)
    {
        auto* const next = c->_next;
        cont_free_cell(c);
        c = next;
    }
    _spill = nullptr;
    // the inline weak_ptrs auto-destroy (dec_weak) as this object is destroyed
}

cc::impl::async_cont_head::async_cont_head(async_cont_head&& o) noexcept : _spill(o._spill)
{
    o._spill = nullptr;
    for (cc::isize i = 0; i < inline_capacity; ++i)
        _inline_deps[i] = cc::move(o._inline_deps[i]);
}

cc::impl::async_cont_head& cc::impl::async_cont_head::operator=(async_cont_head&& o) noexcept
{
    if (this != &o)
    {
        for (auto* c = _spill; c != nullptr;) // release our current spill
        {
            auto* const next = c->_next;
            cont_free_cell(c);
            c = next;
        }
        _spill = o._spill;
        o._spill = nullptr;
        for (cc::isize i = 0; i < inline_capacity; ++i)
            _inline_deps[i] = cc::move(o._inline_deps[i]); // weak_ptr move-assign resets ours, steals o's
    }
    return *this;
}

void cc::impl::async_cont_head::add(async_node_base* dependent)
{
    for (auto& w : _inline_deps)
        if (w.get() == nullptr) // free inline slot: no allocation for the common small fan-out
        {
            w = async_node_weak::from_alive(dependent); // dependent is alive (it is polling us)
            return;
        }

    auto* c = cont_alloc_cell(); // 4th+ dependent: prepend a weak spill cell
    c->_fn = nullptr;
    new (cc::placement_new, &c->_weak) async_node_weak(async_node_weak::from_alive(dependent));
    c->_next = _spill;
    _spill = c;
}

void cc::impl::async_cont_head::add_latch(void (*fn)(void*), void* ctx)
{
    auto* c = cont_alloc_cell(); // latches always spill (rare; the pool blocking driver only)
    c->_fn = fn;
    c->_ctx = ctx;
    c->_next = _spill;
    _spill = c;
}

void cc::impl::async_cont_head::remove(async_node_base* dependent)
{
    // drop the target, and opportunistically prune any dependents that have since expired
    for (auto& w : _inline_deps)
    {
        if (w.get() == nullptr)
            continue;
        auto sp = w.lock();
        if (!sp.is_valid() || sp.get() == dependent)
            w = {};
    }

    async_cont_cell* prev = nullptr;
    for (auto* c = _spill; c != nullptr;)
    {
        auto* const next = c->_next;
        bool drop = false;
        if (c->_fn == nullptr)
        {
            auto sp = c->_weak.lock();
            drop = !sp.is_valid() || sp.get() == dependent;
        }
        if (drop)
        {
            if (prev != nullptr)
                prev->_next = next;
            else
                _spill = next;
            cont_free_cell(c);
        }
        else
            prev = c;
        c = next;
    }
}

cc::isize cc::impl::async_cont_head::count() const
{
    cc::isize n = 0;
    for (auto const& w : _inline_deps)
        if (w.get() != nullptr)
            ++n;
    for (auto* c = _spill; c != nullptr; c = c->_next)
        if (c->_fn == nullptr)
            ++n;
    return n;
}

void cc::impl::async_cont_head::notify_all()
{
    for (auto& w : _inline_deps)
        if (w.get() != nullptr)
            if (auto c = w.lock()) // skips any dependent that is being torn down
                c->schedule();

    for (auto* c = _spill; c != nullptr; c = c->_next)
    {
        if (c->_fn == nullptr)
        {
            if (auto s = c->_weak.lock())
                s->schedule();
        }
        else
            c->_fn(c->_ctx); // fire the completion latch
    }
}

// ============================================================================
// async_node_base — dependency bookkeeping
// ============================================================================

void cc::async_node_base::drop_ready_pending_deps()
{
    _deps.remove_ready();
}

bool cc::async_node_base::try_subscribe(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    if (_state.load(std::memory_order_relaxed) == async_node_state::ready)
        return false;           // already ready under the lock: the dependent must not park on us
    _slot.conts.add(dependent); // dependent is alive (it is polling us)
    return true;
}

bool cc::async_node_base::subscribe_to_pending_deps()
{
    // subscribe to each not-ready dep, marking it via the entry's subscribed bit; the first dep found already
    // ready aborts parking (return true) so the poller re-evaluates from scratch.
    return _deps.for_each_until(
        [this](impl::async_dep_entry e)
        {
            if (e.dep()->try_subscribe(this))
            {
                e.set_subscribed(true);
                return false; // keep going
            }
            return true; // a dep is already ready: abort parking, re-evaluate from scratch
        });
}

void cc::async_node_base::unsubscribe_all()
{
    _deps.for_each(
        [this](impl::async_dep_entry e)
        {
            if (e.subscribed())
            {
                e.dep()->remove_continuation(this);
                e.set_subscribed(false);
            }
        });
}

void cc::async_node_base::add_continuation(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    _slot.conts.add(dependent);
}

void cc::async_node_base::remove_continuation(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    _slot.conts.remove(dependent); // drops the target and prunes any dependents that have since expired
}

// ============================================================================
// async_node_base — completion / teardown
// ============================================================================

cc::async_error cc::async_node_base::propagate_error() const
{
    CC_ASSERT(has_error(), "no error to propagate");
    if (_slot.error.is_cancelled())
        return async_error::make_cancelled();

    // cc::any_error is move-only and a shared node's error must not be moved out, so re-materialize the
    // message. The context chain is lost — a richer error-sharing scheme is a follow-up.
    return async_error::make_error(cc::any_error(_slot.error.underlying().to_string()));
}

bool cc::async_node_base::install_completion_hook_or_ready(void (*fn)(void*), void* ctx)
{
    impl::async_spinlock_guard g(_lock);
    if (_state.load(std::memory_order_relaxed) == async_node_state::ready)
        return true; // already done: caller must not wait
    _slot.conts.add_latch(fn, ctx);
    return false;
}

void cc::async_node_base::mark_ready_and_notify(async_error* err)
{
    _deps.clear();

    impl::async_cont_head continuations; // stolen out of the result slot below
    {
        impl::async_spinlock_guard g(_lock);

        // Steal the continuation head, freeing the result slot; then, for an error completion, switch the
        // slot's active union member to the error (a value completion leaves the value in the typed node and
        // the freed head empty). Publish `ready` LAST, so a subscriber that wins the lock after us sees the
        // value/error already in place (I1); one that wins before us lands in `continuations` and gets woken.
        continuations = cc::move(_slot.conts);
        if (err != nullptr)
        {
            _slot.conts.~async_cont_head();                                    // end the (now empty) head's lifetime
            new (cc::placement_new, &_slot.error) async_error(cc::move(*err)); // begin the error's lifetime
            _is_error = true;
        }
        _state.store(async_node_state::ready, std::memory_order_release);
    }

    // outside the lock: waking a dependent / firing a latch takes other locks, so we must not hold ours
    continuations.notify_all();
}

void cc::async_node_base::complete_from_compute(bool produced_error, async_error& err)
{
    unsubscribe_all(); // still valid: our frame (destroyed below) pins the deps we are unsubscribing from
    _deps.clear();

    destroy_frame(); // release the frame's captures

    mark_ready_and_notify(produced_error ? &err : nullptr);
}

void cc::async_node_base::teardown_payload()
{
    // Base part of the strong-0 teardown: the typed override already ran unsubscribe_all() + released the frame
    // (which pinned the deps) and the value. Free the remaining owned state — spilled dep-list nodes, and the
    // active result-slot member (the continuation head while not ready, else the error once ready-with-error) —
    // but leave the intrusive counts alive for outstanding weak refs. Nothing races us: strong is already 0.
    _deps.clear();
    if (is_ready() && _is_error)
        _slot.error.~async_error();
    else
        _slot.conts.~async_cont_head();
}

// ============================================================================
// async_node_base — the poll loop (never blocks)
// ============================================================================

void cc::async_node_base::poll()
{
    if (!try_begin_running())
        return; // another poller owns it, it is terminal, or it is a manual node awaiting external completion

    unsubscribe_all(); // re-evaluate dependencies from scratch this turn

    async_context ctx;
    ctx.current = this;
    ctx.scheduler = async_scheduler::current_or_null();

    for (;;)
    {
        drop_ready_pending_deps();

        if (!_deps.empty())
        {
            // Eager depth-first drive: require() already made every dependency runnable, so rather than parking
            // we try to satisfy one right here — drive it inline on this stack (better locality, no scheduler
            // round-trip, no wakeup). A work-stealing pool can still steal the other, already-scheduled deps in
            // parallel; whichever finishes first, we re-evaluate and drop it. We only fall back to the old
            // subscribe+park path when the picked dep cannot be completed inline (a manual/push node, one
            // already running on another worker, or one whose own deps aren't ready) or the recursion depth cap
            // is hit. Subscription therefore becomes the exception, not the rule.
            //
            // TODO: in a pure-inline run every dep also sits in the scheduler queue (require() enqueued it) and
            // is popped later as a ready no-op. Harmless, but the enqueue could be gated on "are there
            // steal-capable peers" (a scheduler property) to avoid the churn.
            if (s_inline_depth < async_max_inline_depth)
            {
                async_node_base* const pick = _deps.first(); // non-null: _deps is not empty
                {
                    inline_depth_guard const ds;
                    pick->poll();
                }
                if (pick->is_ready())
                    continue; // progress: drop the finished dep and re-evaluate (drives siblings left-to-right)
                // pick could not be completed inline -> fall through to subscribe + park on the not-ready set
            }

            // Install wakeup continuations late, then decide whether to park.
            bool const found_ready = subscribe_to_pending_deps();

            bool parked = false;
            if (!found_ready)
            {
                impl::async_spinlock_guard g(_lock);
                if (_wake_pending)
                    _wake_pending = false; // a dependency woke us mid-subscribe: don't park, re-evaluate
                else
                {
                    _state.store(async_node_state::blocked, std::memory_order_release);
                    parked = true;
                }
            }

            if (parked)
                return; // continuations installed; a completing dependency will schedule us

            // a dep became ready, or a wake raced in during subscription: unwind and re-evaluate. This is the
            // block-vs-wake race; it cannot lose the wakeup.
            unsubscribe_all();
            continue;
        }

        // A produced error is handed back here (not stored in the node), so the completion path can install it
        // into the result slot only after stealing the still-live continuation head that shares that storage.
        async_error step_error;
        switch (poll_compute_step(ctx, step_error))
        {
        case async_step_status::produced_value:
            complete_from_compute(/*produced_error*/ false, step_error);
            return;
        case async_step_status::produced_error:
            complete_from_compute(/*produced_error*/ true, step_error);
            return;
        case async_step_status::waiting:
            continue; // frame added deps / asked to wait — normalize and poll them now
        case async_step_status::yield:
            reschedule_self();
            return;
        }
    }
}
