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
// The scheduler + served affinity bound to the calling thread (set by async_worker_scope). thread_local even
// without threads: a single-threaded build just has one slot. nullptr scheduler => no worker scope active.
thread_local cc::async_scheduler* s_current_scheduler = nullptr;
thread_local cc::async_affinity s_current_affinity = cc::async_affinity::none();

// Process-wide default scheduler for general-compute nodes that cannot run on the current thread. Read-mostly:
// installed once at startup. Atomic so installation is visible to worker threads without extra synchronization.
std::atomic<cc::async_scheduler*> s_default_scheduler{nullptr};
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

cc::async_affinity cc::async_scheduler::current_affinity()
{
    return s_current_affinity;
}

void cc::async_scheduler::set_default(async_scheduler* sched)
{
    s_default_scheduler.store(sched, std::memory_order_release);
}

cc::async_scheduler* cc::async_scheduler::default_or_null()
{
    return s_default_scheduler.load(std::memory_order_acquire);
}

void cc::async_default_reschedule(std::shared_ptr<async_node_base> node)
{
    // general-compute route: the installed default pool if any, else the current worker (inline driving).
    if (auto* d = async_scheduler::default_or_null())
        d->submit(cc::move(node));
    else if (auto* c = async_scheduler::current_or_null())
        c->enqueue(cc::move(node));
    else
        CC_ASSERT(false, "no scheduler to route a general-compute async: install a default async pool or drive "
                         "it inside an async_worker_scope");
}

cc::async_worker_scope::async_worker_scope(cc::async_scheduler& scheduler, cc::async_affinity served)
  : _previous(s_current_scheduler), _previous_affinity(s_current_affinity)
{
    s_current_scheduler = &scheduler;
    s_current_affinity = served;
}

cc::async_worker_scope::~async_worker_scope()
{
    s_current_scheduler = _previous;
    s_current_affinity = _previous_affinity;
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
// async_node_base — affinity
// ============================================================================

void cc::async_node_base::set_affinity(async_affinity a, async_reschedule_fn route)
{
    CC_ASSERT(_state.load(std::memory_order_relaxed) == async_node_state::cold, "affinity may only be set before the "
                                                                                "async is scheduled");
    CC_ASSERT(route != nullptr, "a user-defined affinity needs a reschedule route to its pool");
    _affinity = a;
    _reschedule = route;
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
        target.submit(shared_from_this());
}

void cc::async_node_base::route_after_schedule()
{
    // State is `scheduled` and nobody else will enqueue it (schedule() is idempotent on `scheduled`), so we
    // route exactly once. Local hot path when a compatible worker scope is active here; else the node's own
    // affinity route (thread-independent), which is what makes cross-thread wakeups correct.
    auto self = shared_from_this();
    if (auto* sched = async_scheduler::current_or_null();
        sched != nullptr && _affinity.overlaps(async_scheduler::current_affinity()))
    {
        sched->enqueue(cc::move(self));
        return;
    }

    CC_ASSERT(_reschedule != nullptr, "a schedulable async must have a reschedule route (compute nodes get a "
                                      "default; a push/manual node must not be scheduled)");
    _reschedule(cc::move(self));
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
// async_node_base — dependency bookkeeping
// ============================================================================

void cc::async_node_base::drop_ready_pending_deps()
{
    _pending_deps.remove_all_where([](async_node_base* d) { return d->is_ready(); });
}

bool cc::async_node_base::try_subscribe(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    if (_state.load(std::memory_order_relaxed) == async_node_state::ready)
        return false; // already ready under the lock: the dependent must not park on us
    _continuations.push_back(dependent->weak_from_this());
    return true;
}

bool cc::async_node_base::subscribe_to_pending_deps()
{
    _subscribed.clear();
    for (auto* dep : _pending_deps)
    {
        if (dep->try_subscribe(this))
            _subscribed.push_back(dep);
        else
            return true; // a dep is already ready: abort parking, re-evaluate from scratch
    }
    return false;
}

void cc::async_node_base::unsubscribe_all()
{
    for (auto* dep : _subscribed)
        dep->remove_continuation(this);
    _subscribed.clear();
}

void cc::async_node_base::add_continuation(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    _continuations.push_back(dependent->weak_from_this());
}

void cc::async_node_base::remove_continuation(async_node_base* dependent)
{
    impl::async_spinlock_guard g(_lock);
    // drop the target, and opportunistically prune any dependents that have since expired
    _continuations.remove_all_where(
        [&](std::weak_ptr<async_node_base> const& w)
        {
            auto sp = w.lock();
            return !sp || sp.get() == dependent;
        });
}

// ============================================================================
// async_node_base — completion / teardown
// ============================================================================

cc::async_error cc::async_node_base::propagate_error() const
{
    CC_ASSERT(has_error(), "no error to propagate");
    if (_error.is_cancelled())
        return async_error::make_cancelled();

    // cc::any_error is move-only and a shared node's error must not be moved out, so re-materialize the
    // message. The context chain is lost — a richer error-sharing scheme is a follow-up.
    return async_error::make_error(cc::any_error(_error.underlying().to_string()));
}

bool cc::async_node_base::install_completion_hook_or_ready(void (*fn)(void*), void* ctx)
{
    impl::async_spinlock_guard g(_lock);
    if (_state.load(std::memory_order_relaxed) == async_node_state::ready)
        return true; // already done: caller must not wait
    _on_complete = fn;
    _on_complete_ctx = ctx;
    return false;
}

void cc::async_node_base::mark_ready_and_notify()
{
    _pending_deps.clear();

    cc::small_vector<std::weak_ptr<async_node_base>, 1> continuations;
    void (*on_complete)(void*) = nullptr;
    void* on_complete_ctx = nullptr;
    {
        impl::async_spinlock_guard g(_lock);
        _state.store(async_node_state::ready, std::memory_order_release); // ready store under the lock (I1)

        // detach then wake: a woken dependent may re-poll and re-subscribe elsewhere
        continuations = cc::move(_continuations);
        _continuations.clear();
        on_complete = _on_complete;
        on_complete_ctx = _on_complete_ctx;
    }

    // outside the lock: waking a dependent takes its lock, so we must not hold ours (no two node locks at once)
    for (auto const& w : continuations)
        if (auto c = w.lock()) // skips any dependent that is being torn down
            c->schedule();

    if (on_complete != nullptr)
        on_complete(on_complete_ctx);
}

void cc::async_node_base::complete_from_compute()
{
    unsubscribe_all(); // still valid: our frame (destroyed below) pins the deps we are unsubscribing from
    _pending_deps.clear();

    destroy_frame(); // release the frame's captures

    mark_ready_and_notify();
}

cc::async_node_base::~async_node_base()
{
    // The typed node destructor already ran unsubscribe_all() before dropping its frame. This is a defensive
    // backstop for any node destroyed without a value (e.g. an undriven manual node).
    unsubscribe_all();
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

        if (!_pending_deps.empty())
        {
            // Not-ready dependencies remain (require() already scheduled any cold ones). Install wakeup
            // continuations late, then decide whether to park.
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

        switch (poll_compute_step(ctx))
        {
        case async_step_status::produced_value:
        case async_step_status::produced_error:
            complete_from_compute();
            return;
        case async_step_status::waiting:
            continue; // frame added deps / asked to wait — normalize and poll them now
        case async_step_status::yield:
            reschedule_self();
            return;
        }
    }
}
