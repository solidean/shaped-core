#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>

// Untemplated core of the async runtime: the per-thread scheduler binding, the singlethreaded scheduler pump,
// and the node state machine / poll loop. See async_node.hh for the shape and invariants.
//
// Concurrency model (safe to drive from many threads):
//   * a per-node spinlock (the low bit of the packed state/ops control word) serializes state transitions and
//     continuation/subscription bookkeeping;
//   * that word stays atomic for lock-free is_ready()/is_cold() reads;
//   * at most one thread polls a node (try_begin_running); a completing dependency that wakes a running node
//     sets the wake-pending bit instead of enqueuing a second copy, and the active poller re-polls;
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

void cc::singlethreaded_scheduler::enqueue(async_node_ptr node)
{
    _queue.push_back(cc::move(node));
}

bool cc::singlethreaded_scheduler::run_one()
{
    if (_queue.empty())
        return false;

    auto node = cc::move(_queue.back()); // keep it alive across the poll, then release our queue ref
    _queue.remove_back();
    node->poll();
    return true;
}

void cc::singlethreaded_scheduler::run_until(cc::function_ref<bool()> done)
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
        lock_scope g(this);
        auto const s = load_state(std::memory_order_relaxed);

        // terminal, already runnable, or a manual node (only external completion makes those ready)
        if (is_ready_state(s) || s == async_node_state::scheduled || s == async_node_state::external_pending)
            return;

        if (s == async_node_state::running)
        {
            // a second poller must never run this node: record a re-poll request; the active poller reconciles
            // at its next park point instead of parking.
            set_wake();
            return;
        }

        // cold or blocked -> make runnable (we route exactly once, below, after releasing the lock)
        store_state(async_node_state::scheduled);
    }

    route_after_schedule();
}

void cc::async_node_base::schedule_on(async_scheduler& target)
{
    bool do_submit = false;
    {
        lock_scope g(this);
        auto const s = load_state(std::memory_order_relaxed);

        if (is_ready_state(s) || s == async_node_state::scheduled || s == async_node_state::external_pending)
            return; // terminal, already runnable elsewhere, or a manual node

        if (s == async_node_state::running)
        {
            set_wake();
            return;
        }

        store_state(async_node_state::scheduled);
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
    lock_scope g(this);
    auto const s = load_state(std::memory_order_relaxed);

    // another poller owns it, it is terminal, or it awaits external completion -> not runnable here
    if (is_ready_state(s) || s == async_node_state::running || s == async_node_state::external_pending)
        return false;

    store_state_clear_wake(async_node_state::running); // start fresh; any wake during this run re-sets it
    return true;
}

void cc::async_node_base::reschedule_self()
{
    // yield: the one legitimate self-driven running -> scheduled transition. Bypasses the wake-suppression in
    // schedule() (which would leave a running node un-enqueued). A yield stays on the current, compatible
    // worker, so route_after_schedule takes the local hot path.
    {
        lock_scope g(this);
        CC_ASSERT(load_state(std::memory_order_relaxed) == async_node_state::running, "yield from a non-running node");
        store_state_clear_wake(async_node_state::scheduled);
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

void cc::impl::async_dep_head::remove_ready_slow()
{
    // empty is handled by the inline remove_ready guard; here _head != 0.
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
// async_cont_head — continuation set (one tagged word: inline dependent | spill list; see async_node.hh)
// ============================================================================

void cc::impl::async_cont_head::spill_inline()
{
    // The inline slot cannot hold a 2nd entry or a latch, so its dependent moves into a cell first. adopt()
    // takes over the weak count we were holding by hand — no inc/dec pair, and the cell owns it from here.
    auto* c = cont_alloc_cell();
    c->_fn = nullptr;
    new (cc::placement_new, &c->_weak) async_node_weak(async_node_weak::adopt(inline_dep()));
    c->_next = nullptr;
    set_list_head(c);
}

void cc::impl::async_cont_head::normalize()
{
    if ((_head & tag_is_list) == 0)
        return; // empty or inline already

    if (list_head() == nullptr)
        _head = 0; // list emptied (remove() can leave a null list head, i.e. the bare tag)
    // NOTE: a 1-entry list is deliberately NOT collapsed back to the inline slot. Unlike async_dep_head's
    // normalize (on the poll loop's hot path), remove() is rare, and a latch cell cannot live inline at all.
}

void cc::impl::async_cont_head::clear()
{
    // empty is handled by the destructor's inline guard; here _head != 0.
    if ((_head & tag_is_list) == 0)
    {
        release_inline();
    }
    else
    {
        for (auto* c = list_head(); c != nullptr;)
        {
            auto* const next = c->_next;
            cont_free_cell(c);
            c = next;
        }
    }
    _head = 0;
}

void cc::impl::async_cont_head::add(async_node_base* dependent)
{
    if (_head == 0) // empty -> inline (bit0 == 0 => not a list); no allocation for a single dependent
    {
        // dependent is alive (it is polling us); release() hands its weak count to the inline slot
        _head = reinterpret_cast<cc::u64>(async_node_weak::from_alive(dependent).release());
        return;
    }

    if ((_head & tag_is_list) == 0)
        spill_inline(); // 2nd dependent: the inline entry moves into the list first

    auto* c = cont_alloc_cell(); // prepend a weak cell
    c->_fn = nullptr;
    new (cc::placement_new, &c->_weak) async_node_weak(async_node_weak::from_alive(dependent));
    c->_next = list_head();
    set_list_head(c);
}

void cc::impl::async_cont_head::add_latch(void (*fn)(void*), void* ctx)
{
    // latches always live in cells (rare; the pool blocking driver only), so an inline dependent moves first
    if (_head != 0 && (_head & tag_is_list) == 0)
        spill_inline();

    auto* c = cont_alloc_cell();
    c->_fn = fn;
    c->_ctx = ctx;
    c->_next = list_head(); // null in empty mode: _head 0 decodes to a null list head
    set_list_head(c);
}

void cc::impl::async_cont_head::remove(async_node_base* dependent)
{
    // drop the target, and opportunistically prune any dependents that have since expired
    if (_head == 0)
        return;

    if ((_head & tag_is_list) == 0)
    {
        auto w = async_node_weak::adopt(inline_dep()); // borrow our hand-held ref for the liveness test
        auto const sp = w.lock();
        if (!sp.is_valid() || sp.get() == dependent)
            _head = 0; // dropped: w's destructor pays the dec_weak
        else
            _head = reinterpret_cast<cc::u64>(w.release()); // kept: hand the ref back to the inline slot
        return;
    }

    async_cont_cell* prev = nullptr;
    for (auto* c = list_head(); c != nullptr;)
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
                set_list_head(next);
            cont_free_cell(c);
        }
        else
            prev = c;
        c = next;
    }
    normalize(); // set_list_head(nullptr) above leaves the bare tag; this repairs it to empty
}

cc::isize cc::impl::async_cont_head::count() const
{
    if (_head == 0)
        return 0;
    if ((_head & tag_is_list) == 0)
        return 1;

    cc::isize n = 0;
    for (auto* c = list_head(); c != nullptr; c = c->_next)
        if (c->_fn == nullptr)
            ++n;
    return n;
}

void cc::impl::async_cont_head::notify_all()
{
    if (_head == 0)
        return;

    if ((_head & tag_is_list) == 0)
    {
        auto w = async_node_weak::adopt(inline_dep()); // borrowed: handed straight back below
        if (auto const s = w.lock())                   // skips a dependent that is being torn down
            s->schedule();
        _head = reinterpret_cast<cc::u64>(w.release()); // notify_all does not consume — our dtor still owes it
        return;
    }

    for (auto* c = list_head(); c != nullptr; c = c->_next)
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
    deps().remove_ready();
}

void cc::async_node_base::schedule_pending_deps(async_node_base* except)
{
    // Only COLD deps: those are the ones nobody has taken responsibility for yet. A dep that is already
    // scheduled/running is accounted for, and one that is `blocked` is parked on its OWN deps — schedule()
    // would drag it back to `scheduled` and re-enqueue it, and it would just re-subscribe and re-park. Down a
    // chain past the inline depth cap that turns every park into a re-poll storm.
    deps().for_each(
        [except](impl::async_dep_entry e)
        {
            if (e.dep() != except && e.dep()->is_cold())
                e.dep()->schedule();
        });
}

bool cc::async_node_base::try_subscribe(async_node_base* dependent)
{
    lock_scope g(this);
    if (is_ready_state(load_state(std::memory_order_relaxed)))
        return false;       // already ready under the lock: the dependent must not park on us
    conts().add(dependent); // dependent is alive (it is polling us)
    return true;
}

bool cc::async_node_base::subscribe_to_pending_deps()
{
    // subscribe to each not-ready dep, marking it via the entry's subscribed bit; the first dep found already
    // ready aborts parking (return true) so the poller re-evaluates from scratch.
    return deps().for_each_until(
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

void cc::async_node_base::unsubscribe_all_slow()
{
    // empty deps are handled by the inline unsubscribe_all guard; here the set is non-empty.
    deps().for_each(
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
    lock_scope g(this);
    CC_ASSERT(!is_ready_state(load_state(std::memory_order_relaxed)), "add_continuation on a ready node: the "
                                                                      "continuation head has been stolen");
    conts().add(dependent);
}

void cc::async_node_base::remove_continuation(async_node_base* dependent)
{
    lock_scope g(this);
    if (is_ready_state(load_state(std::memory_order_relaxed)))
        return;                // completed: the continuation head was stolen and its storage may now hold the error
    conts().remove(dependent); // drops the target and prunes any dependents that have since expired
}

// ============================================================================
// async_node_base — completion / teardown
// ============================================================================

cc::async_error cc::impl::async_error_propagate(async_error const& e)
{
    if (e.is_cancelled())
        return async_error::make_cancelled();

    // cc::any_error is move-only and a shared node's error must not be moved out, so re-materialize the
    // message. The context chain is lost — a richer error-sharing scheme is a follow-up.
    return async_error::make_error(cc::any_error(e.underlying().to_string()));
}

bool cc::async_node_base::install_completion_hook_or_ready(void (*fn)(void*), void* ctx)
{
    lock_scope g(this);
    if (is_ready_state(load_state(std::memory_order_relaxed)))
        return true; // already done: caller must not wait
    conts().add_latch(fn, ctx);
    return false;
}

void cc::async_node_base::teardown_payload()
{
    // Strong-0 teardown (nothing races us — strong is already 0). If ready, the unresolved arm is already gone
    // and the payload holds the resolved value/error — destroy that (typed, via the ops table); else the arm is
    // live, so unsubscribe (the frame still pins the deps) then destroy the whole arm (frame + deps + conts). The
    // intrusive counts and _ops stay alive for outstanding weak refs; free_storage reclaims the raw node later.
    auto const s = load_state(std::memory_order_relaxed);
    if (s == async_node_state::ready_value)
    {
        if (auto const f = ops()->teardown_value) // null for a trivially-destructible value type
            f(this);                              // destroy the resolved value at payload offset 0
    }
    else if (s == async_node_state::ready_error)
    {
        if (auto const f = ops()->teardown_error) // null for a trivially-destructible error type
            f(this);                              // destroy the resolved error at payload offset 0
    }
    else
    {
        unsubscribe_all();
        destroy_frame();                  // a never-resolved frame (dropped cold, or parked) — a plain ~F, so
                                          // no re-entrancy contract applies here; it just releases its captures
        unresolved().~async_unresolved(); // deps + conts
    }
}

// ============================================================================
// async_node_base — the poll loop (never blocks)
// ============================================================================

void cc::async_node_base::poll()
{
    if (!try_begin_running())
        return; // another poller owns it, it is terminal, or it is a manual node awaiting external completion

    unsubscribe_all(); // re-evaluate dependencies from scratch this turn

    async_context_base ctx;
    ctx.current = this;
    ctx.scheduler = async_scheduler::current_or_null();

    for (;;)
    {
        drop_ready_pending_deps();

        if (!deps().empty())
        {
            // Eager depth-first drive: rather than parking we try to satisfy one dependency right here — drive
            // it inline on this stack (better locality, no scheduler round-trip, no wakeup). We only fall back
            // to the subscribe+park path when the picked dep cannot be completed inline (a manual/push node,
            // one already running on another worker, or one whose own deps aren't ready) or the recursion depth
            // cap is hit. Subscription therefore becomes the exception, not the rule.
            if (s_inline_depth < async_max_inline_depth)
            {
                async_node_base* const pick = deps().first(); // non-null: deps() is not empty

                // Publish all-but-one: `pick` runs here, so enqueuing it would only churn — it would be popped
                // later as a ready no-op, and until then the queue's strong ref pins it alive. The siblings are
                // worth publishing only if someone could actually steal them; a singlethreaded scheduler has no
                // peers, so it publishes nothing and drives the whole graph on this stack.
                if (ctx.scheduler != nullptr && ctx.scheduler->has_steal_capable_peers)
                    schedule_pending_deps(pick);

                {
                    inline_depth_guard const ds;
                    pick->poll();
                }
                if (pick->is_ready())
                    continue; // progress: drop the finished dep and re-evaluate (drives siblings left-to-right)
                // pick could not be completed inline -> fall through to subscribe + park on the not-ready set
            }

            // About to park, so nothing on this stack will drive them: every remaining dep must be runnable
            // now, or nobody ever wakes us. require() no longer does this, and the depth-cap path above skips
            // the inline drive entirely.
            schedule_pending_deps(nullptr);

            // Install wakeup continuations late, then decide whether to park.
            bool const found_ready = subscribe_to_pending_deps();

            bool parked = false;
            if (!found_ready)
            {
                lock_scope g(this);
                if (wake_pending())
                    clear_wake(); // a dependency woke us mid-subscribe: don't park, re-evaluate
                else
                {
                    store_state(async_node_state::blocked);
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

        // Run the compute step with the frame in place — it is never moved, so parking is free and a stateful
        // (mutable) closure just picks up where it left off. If it resolves (value OR error) it has already
        // destroyed itself: finish_value/finish_error builds the result over the frame's own slot.
        CC_ASSERT(ops()->frame_invoke != nullptr, "polled a node without a compute frame");
        switch (ops()->frame_invoke(frame_storage(), ctx))
        {
        case async_step_status::produced_value:
        case async_step_status::produced_error:
            return; // resolve_to_value/error already completed the node in place + woke dependents
        case async_step_status::waiting:
            continue; // frame added deps / asked to wait — normalize and poll them now
        case async_step_status::yield:
            reschedule_self();
            return;
        }
    }
}
