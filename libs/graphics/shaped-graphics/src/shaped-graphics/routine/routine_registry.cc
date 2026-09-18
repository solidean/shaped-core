#include <clean-core/common/assertf.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-graphics/routine/routine_registry.hh>

namespace sg
{
cc::vector<std::shared_ptr<render_routine_base>> routine_registry::snapshot()
{
    return _entries.lock(
        [](routine_map& entries)
        {
            auto out = cc::vector<std::shared_ptr<render_routine_base>>::create_with_capacity(entries.size());
            for (auto const& [key, routine] : entries)
            {
                (void)key;
                out.push_back(routine);
            }
            return out;
        });
}

routine_tick_result routine_registry::tick(routine_tick_options const& options)
{
    CC_RECORD_SCOPE("sg.routine.tick");

    // Initialization runs on the ambient async scheduler, and installing one is the application's job.
    // Asserts where there is none rather than standing up a private one: a phase nothing can drive would leave every
    // routine pending forever, which is a configuration error and not a state to report.
    //
    // The exception is a thread-bound context, whose bound calls must all come from one thread: a phase resuming on a pool worker would call it from another.
    // Its phases start on a scheduler of the registry's own, bound to whichever thread ticks and driven only here.
    auto const pins_phases = _ctx.threading() != thread_model::multi_threaded;
    if (pins_phases && _phase_scheduler == nullptr)
        _phase_scheduler = std::make_unique<cc::singlethreaded_scheduler>();
    auto& scheduler = pins_phases ? *_phase_scheduler : cc::ambient_async_scheduler();
    auto const pinned = pins_phases ? std::make_unique<cc::async_worker_scope>(*_phase_scheduler) : nullptr;

    auto const tick_index = ++_ticks;

    auto result = routine_tick_result();

    auto pending = snapshot();
    if (pending.empty())
        return result;

    auto const start = options.clock_seconds();

    // The reload generation is read ONCE, here.
    // That is what keeps a reload from landing in the middle of a frame: everything this tick does belongs to one
    // generation, and the next tick is where a newer one is noticed.
    auto const generation = sg::reload_generation();

    // One list for every routine this tick brings up, so their GPU init work batches into a single submit rather than
    // one per routine.
    // Opened for the whole tick, because a phase resuming on a worker needs a window to still be there.
    auto cmd = _ctx.create_command_list();
    open_window(*cmd);

    for (auto const& routine : pending)
        routine->cancel_init_for_reload_if_stale(generation);

    // Driven until nothing moves or the budget runs out.
    // A phase runs on a worker, so a pass that starts one usually collects it on a later pass rather than immediately.
    while (true)
    {
        // Checked BETWEEN passes, never inside a phase — a phase is not interruptible except where it yields, which
        // is why the budget is documented as pacing rather than a deadline.
        if (options.budget_secs.has_value() && options.clock_seconds() - start >= options.budget_secs.value())
        {
            result.budget_exhausted = true;
            break;
        }

        auto progressed = false;
        for (auto const& routine : pending)
            if (routine->advance_init(*this, _ctx, generation))
            {
                ++result.initialized;
                progressed = true;
            }

        auto any_work = false;
        for (auto const& routine : pending)
            if (routine->needs_init(generation))
                any_work = true;

        if (!any_work)
            break; // everything is either up or failed at this generation

        // Driven here, not merely waited for: under a single-threaded scheduler this thread is the only one that can
        // run a phase at all, and a tick that only pumped would spin against a queue nobody empties.
        // A context that cannot block leaves instead of yielding: what a phase waits on arrives only after this returns.
        if (!progressed && !scheduler.try_run_one() && !cc::thread_pump_all())
        {
            if (_ctx.execution() == execution_model::never_block)
                break;
            cc::this_thread_yield();
        }
    }

    close_window();
    (void)_ctx.submit_command_list(cc::move(cmd));

    for (auto const& routine : pending)
        if (routine->own_readiness() == routine_readiness::pending)
        {
            ++result.pending;
            routine->note_pending_at(tick_index);
        }
        else
            routine->clear_pending_mark();

    return result;
}

void routine_registry::open_window(command_list& cmd)
{
    auto waiting = _window.lock(
        [&](window_state& w)
        {
            w.cmd = &cmd;
            auto gate = cc::move(w.gate);
            w.gate = {};
            return gate;
        });

    // Settled OUTSIDE the lock: whoever was parked resumes here, and a resumption that reached back into the window
    // would deadlock on a mutex this thread still held.
    if (waiting != nullptr)
        waiting->push_value(cc::unit{});
}

void routine_registry::close_window()
{
    _window.lock([](window_state& w) { w.cmd = nullptr; });
}

cc::shared_async<cc::unit> routine_registry::next_window()
{
    auto gate = _window.lock(
        [](window_state& w)
        {
            if (w.gate == nullptr)
                w.gate = cc::make_async_manual<cc::unit>();
            return w.gate;
        });

    // Whoever waits between ticks has to tick again for this phase to move, so it is told.
    // Fired outside the window lock, since the waiter it wakes goes straight into a tick that takes it.
    auto const signal = _progress.lock([](std::shared_ptr<impl::routine_progress_signal>& p) { return p; });
    if (signal != nullptr)
        signal->fire();
    return gate;
}

namespace
{
/// Fire `signal` once `phase` settles, however it settles.
cc::shared_async<cc::unit> fire_when_settled(cc::shared_async<cc::unit> phase,
                                             std::shared_ptr<impl::routine_progress_signal> signal)
{
    co_await cc::async_settled(phase);
    signal->fire();
    co_return;
}
} // namespace

cc::vector<cc::shared_async<cc::unit>> routine_registry::in_flight_phases()
{
    auto out = cc::vector<cc::shared_async<cc::unit>>();
    for (auto const& routine : snapshot())
        routine->_init.lock(
            [&](render_routine_base::init_state& s)
            {
                if (s.in_flight != nullptr)
                    out.push_back(s.in_flight);
            });
    return out;
}

cc::shared_async<routine_tick_result> routine_registry::idle_completion()
{
    auto node = idle_completion_steps();
    if (auto* const home = _ctx.device_home())
        (void)node->try_home_cold(*home);
    return node;
}

cc::shared_async<routine_tick_result> routine_registry::idle_completion_steps()
{
    auto const signal = std::make_shared<impl::routine_progress_signal>();
    _progress.lock([&](std::shared_ptr<impl::routine_progress_signal>& p) { p = signal; });

    // One watcher per running phase, kept until the end: a watcher still finishing when this settles would outlive
    // the caller that awaited it.
    struct watched_phase
    {
        cc::shared_async<cc::unit> phase;
        cc::shared_async<cc::unit> watcher;
    };
    auto watched = cc::vector<watched_phase>();

    auto total = routine_tick_result();
    while (true)
    {
        // Armed BEFORE the tick: a phase that settles or asks for a window while the tick runs must not be lost to the
        // gap between the tick returning and this parking.
        auto const woken = signal->arm();

        auto const pass = tick();
        total.initialized += pass.initialized;
        total.pending = pass.pending;

        // A routine's init may register another one, which only the next tick sees.
        if (pass.initialized > 0)
            continue;
        if (pass.pending == 0)
            break;

        // A thread-bound context's phases run on the registry's own scheduler, which only a tick drives.
        if (_phase_scheduler != nullptr && !_phase_scheduler->empty())
            continue;

        // Pending with nothing running: a tick collected a phase and left before starting the next one, or a reload
        // landed mid-tick.
        // Either way the next tick starts what is pending, so this goes straight back rather than parking on a signal
        // nothing would fire.
        auto const phases = in_flight_phases();
        if (phases.empty())
            continue;

        for (auto const& phase : phases)
        {
            auto known = false;
            for (auto const& w : watched)
                known |= w.phase == phase;
            if (!known)
                watched.push_back({.phase = phase, .watcher = cc::async_start(fire_when_settled(phase, signal))});
        }
        co_await woken;
    }

    _progress.lock([](std::shared_ptr<impl::routine_progress_signal>& p) { p = nullptr; });

    // A settled phase's watcher is running or about to, so it finishes first; one whose phase never settled (a
    // reload abandoned it) is simply dropped.
    for (auto const& w : watched)
        if (w.phase->is_ready())
            co_await cc::async_settled(w.watcher);
    co_return total;
}

void routine_registry::add_dependency(render_routine_base const* from, std::shared_ptr<render_routine_base> to)
{
    CC_ASSERT(from != nullptr && to != nullptr, "a routine dependency needs both ends");
    CC_ASSERT(from != to.get(), "a routine cannot depend on itself");

    _edges.lock(
        [&](edge_map& edges)
        {
            // Walk forward from the new edge's target looking for its source.
            // Reaching it means this edge closes a cycle: readiness would never settle, because each end waits for the
            // other, and initialization would deadlock taking the two locks in opposite orders.
            // An assert rather than a failure because a cycle is a programming error and there is no recovery from it
            // -- we may relax that to a permanent failure later if it turns out to cost stability.
            auto stack = cc::vector<render_routine_base const*>();
            auto seen = cc::vector<render_routine_base const*>();
            stack.push_back(to.get());
            while (!stack.empty())
            {
                auto const* const at = stack.back();
                stack.remove_back();

                CC_ASSERT(at != from, "a render-routine dependency cycle: the routine being depended on already "
                                      "depends on the one declaring it, directly or through another routine");

                auto already = false;
                for (auto const* s : seen)
                    if (s == at)
                    {
                        already = true;
                        break;
                    }
                if (already)
                    continue;
                seen.push_back(at);

                if (auto* const next = edges.get_ptr(at))
                    for (auto const& n : *next)
                        stack.push_back(n.get());
            }

            auto& list = edges[from];
            for (auto const& existing : list)
                if (existing.get() == to.get())
                    return; // already recorded; declaring the same dependency twice is ordinary
            list.push_back(cc::move(to));
        });
}

void routine_registry::drop_edges_from(render_routine_base const* routine)
{
    if (routine == nullptr)
        return;
    _edges.lock([routine](edge_map& edges) { edges.erase(routine); });
}

void routine_registry::warn_if_never_ticked()
{
    // Threshold rather than the first acquire: a routine registered and asked about in the same frame is ordinary, and
    // the tick that would bring it up has simply not come round yet.
    //
    // Low enough that a TEST reaches it.
    // At a thousand it was a diagnostic only a long-running application could trigger, which is the one case where
    // the empty screen is already obvious.
    // A suite that declines every draw and says nothing is the case it is actually worth having.
    //
    // This counter is per REGISTRY rather than per routine, which is what sets the floor: a renderer with many
    // routines spends acquires quickly and legitimately.
    // One parametrized routine used at sixteen sites in the first frames would already be there, so the threshold has
    // to clear a whole frame's worth of honest acquires before it means anything.
    static constexpr auto k_acquires_before_warning = u64(128);

    if (_ticks.load(cc::memory_order_relaxed) != 0)
        return;
    if (++_pending_acquires < k_acquires_before_warning)
        return;
    if (_warned_never_ticked.exchange(true))
        return;

    CC_LOG_WARNING("routines have been asked for {} times and ctx.routines.tick() has never run — nothing will ever "
                   "be ready. Call it once per frame, after advance_epoch and before the frame's first acquire",
                   k_acquires_before_warning);
}

void routine_registry::warn_if_pending_too_long(render_routine_base& routine)
{
    // The second half of the never-ticked diagnostic, and the more useful one: by the time someone hits THIS they
    // have already called tick, so the obvious explanation is spent and there is nothing else to read.
    //
    // Counted in ticks rather than in acquires, because that is the thing that was supposed to make progress.
    static constexpr auto k_ticks_before_warning = u64(600); // ~10 s at 60 Hz, well past any honest compile

    auto const ticks = _ticks.load(cc::memory_order_relaxed);
    if (ticks < k_ticks_before_warning)
        return;

    auto const first = routine.first_pending_tick();
    if (first == 0 || ticks - first < k_ticks_before_warning)
        return;
    if (routine.mark_warned_pending())
        return; // already said, once, for this routine

    CC_LOG_WARNING("a render routine has been pending for {} ticks — it or something it depends on is not coming up. "
                   "Check that its shader package is registered and that its shaders compile",
                   ticks - first);
}

routine_readiness routine_registry::readiness_of(render_routine_base& routine)
{
    auto const own = routine.own_readiness();
    if (own == routine_readiness::pending)
    {
        warn_if_never_ticked();
        warn_if_pending_too_long(routine);
    }
    if (own == routine_readiness::failed)
        return routine_readiness::failed;

    // The subtree, iteratively: a dependency that is not up makes the holder not up either.
    // add_dependency refuses cycles, so this terminates -- the `seen` list is there for a diamond rather than a loop.
    auto worst = own;
    auto stack = cc::vector<render_routine_base*>();
    auto seen = cc::vector<render_routine_base const*>();

    auto const push_children = [&](render_routine_base const* at)
    {
        _edges.lock(
            [&](edge_map& edges)
            {
                if (auto* const next = edges.get_ptr(at))
                    for (auto const& n : *next)
                        stack.push_back(n.get());
            });
    };

    push_children(&routine);
    while (!stack.empty())
    {
        auto* const at = stack.back();
        stack.remove_back();

        auto already = false;
        for (auto const* s : seen)
            if (s == at)
            {
                already = true;
                break;
            }
        if (already)
            continue;
        seen.push_back(at);

        auto const child = at->own_readiness();
        if (child == routine_readiness::failed)
            return routine_readiness::failed;
        if (child == routine_readiness::pending)
            worst = routine_readiness::pending;

        push_children(at);
    }

    return worst;
}

void routine_registry::clear()
{
    // Detached work first, and OUTSIDE the map lock: a pipeline build no phase owns still references the context this
    // clear is part of tearing down.
    // Waiting here is what makes "the context outlives everything started against it" true rather than usually true.
    // Outside the lock because the wait blocks, and a routine registering another one while we hold `_entries` would
    // deadlock against its own initialization.
    // A context destroyed during static teardown has no ambient scheduler left; that is safe because an empty backlog
    // hands back a resolved node, and blocking on one needs none.
    auto const settled = _ctx.backlog.settled();
    if (_ctx.execution() == execution_model::never_block)
        CC_ASSERT(settled->is_ready(),
                  "clearing routines would wait for background work, and this context cannot — await "
                  "ctx.backlog.settled() before shutting it down");
    else
        (void)cc::try_async_blocking_get(settled);

    // Edges next: a token holds a strong reference, so a cycle that slipped past add_dependency would otherwise keep
    // its own routines alive after the map let go of them.
    _edges.lock([](edge_map& edges) { edges.clear(); });
    _entries.lock([](routine_map& entries) { entries.clear(); });
}
} // namespace sg
