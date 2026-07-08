#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_node.hh>

// Untemplated core of the async runtime: the per-thread scheduler binding, the inline scheduler pump, and
// the node state machine / poll loop. See async_node.hh for the shape and invariants.

namespace
{
// The scheduler bound to the calling thread (set by async_worker_scope). thread_local even without threads:
// a single-threaded build just has one slot. nullptr means "no worker scope active".
thread_local cc::async_scheduler* s_current_scheduler = nullptr;
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

cc::async_worker_scope::async_worker_scope(cc::async_scheduler& scheduler) : _previous(s_current_scheduler)
{
    s_current_scheduler = &scheduler;
}

cc::async_worker_scope::~async_worker_scope()
{
    s_current_scheduler = _previous;
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
    auto s = _state.load(std::memory_order_acquire);
    for (;;)
    {
        // already terminal, or already runnable/queued
        if (s == async_node_state::ready || s == async_node_state::scheduled)
            return;

        // cold / running / blocked / external_pending -> make runnable and enqueue.
        // Note: scheduling a currently-running node just enqueues it; the running poller reconciles at its
        // next park/yield/complete. Single-threaded, this is race-free; a future threaded scheduler must
        // additionally prevent a second poller from popping a logically-running node.
        if (_state.compare_exchange_weak(s, async_node_state::scheduled))
        {
            if (auto* sched = async_scheduler::current_or_null())
                sched->enqueue(shared_from_this()); // the queue co-owns the node until it is polled
            return;
        }
    }
}

bool cc::async_node_base::try_begin_running()
{
    auto s = _state.load(std::memory_order_acquire);
    for (;;)
    {
        // another poller owns it, it is terminal, or it awaits external completion -> not runnable here
        if (s == async_node_state::ready || s == async_node_state::running || s == async_node_state::external_pending)
            return false;

        if (_state.compare_exchange_weak(s, async_node_state::running))
            return true;
    }
}

void cc::async_node_base::reschedule_self()
{
    // yield: become runnable again for a later turn
    schedule();
}

// ============================================================================
// async_node_base — dependency bookkeeping
// ============================================================================

void cc::async_node_base::drop_ready_pending_deps()
{
    _pending_deps.remove_all_where([](async_node_base* d) { return d->is_ready(); });
}

void cc::async_node_base::subscribe_to_pending_deps()
{
    _subscribed.clear();
    for (auto* dep : _pending_deps)
    {
        dep->add_continuation(this);
        _subscribed.push_back(dep);
    }
}

void cc::async_node_base::unsubscribe_all()
{
    for (auto* dep : _subscribed)
        dep->remove_continuation(this);
    _subscribed.clear();
}

void cc::async_node_base::add_continuation(async_node_base* dependent)
{
    CC_ASSERT(!_single_consumer || _continuations.empty(), "once_async allows only a single dependent (it is "
                                                           "single-consumer)");
    _continuations.push_back(dependent);
}

void cc::async_node_base::remove_continuation(async_node_base* dependent)
{
    _continuations.remove_all_value(dependent);
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

void cc::async_node_base::destroy_children()
{
    _children.clear();
}

void cc::async_node_base::mark_ready_and_notify()
{
    _pending_deps.clear();
    _state.store(async_node_state::ready, std::memory_order_release);

    // detach then wake: a woken dependent may re-poll and re-subscribe elsewhere
    auto continuations = cc::move(_continuations);
    _continuations.clear();
    for (auto* c : continuations)
        c->schedule();
}

void cc::async_node_base::complete_from_compute()
{
    unsubscribe_all();
    _pending_deps.clear();

    // release captures: owned children first, then this frame (a child may borrow parent-frame state)
    destroy_children();
    destroy_frame();

    mark_ready_and_notify();
}

cc::async_node_base::~async_node_base()
{
    // The typed node destructor already ran unsubscribe_all() + destroy_children() before dropping its frame.
    // This is a defensive backstop for any node destroyed without a value (e.g. an undriven manual node).
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
            // Not-ready dependencies remain (require() already scheduled any cold ones, so they are runnable
            // and will be driven by the scheduler). Install wakeup continuations late, then park.
            subscribe_to_pending_deps();

            auto expected = async_node_state::running;
            if (_state.compare_exchange_strong(expected, async_node_state::blocked))
                return; // parked; a completing dependency will schedule us

            // a wake raced in during subscription (we were re-scheduled and enqueued): unwind and let the
            // scheduler re-poll us. This is the block-vs-wake race; it cannot lose the wakeup.
            CC_ASSERT(expected == async_node_state::scheduled, "unexpected state while parking");
            unsubscribe_all();
            return;
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
