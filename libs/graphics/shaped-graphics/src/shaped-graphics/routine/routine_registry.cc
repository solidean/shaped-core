#include <clean-core/common/assertf.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/thread/async.hh>
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
    auto& scheduler = cc::ambient_async_scheduler();

    auto result = routine_tick_result();

    auto pending = snapshot();
    if (pending.empty())
        return result;

    auto const start = cc::current_time_steady_secs();

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
        if (options.budget_secs.has_value() && cc::current_time_steady_secs() - start >= options.budget_secs.value())
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
        if (!progressed && !scheduler.try_run_one() && !cc::thread_pump_all())
            cc::this_thread_yield();
    }

    close_window();
    (void)_ctx.submit_command_list(cc::move(cmd));

    for (auto const& routine : pending)
        if (routine->own_readiness() == routine_readiness::pending)
            ++result.pending;

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
    return _window.lock(
        [](window_state& w)
        {
            if (w.gate == nullptr)
                w.gate = cc::make_async_manual<cc::unit>();
            return w.gate;
        });
}

routine_tick_result routine_registry::tick_until_idle()
{
    auto total = routine_tick_result();
    // Bounded by the routine count rather than by a timeout: each pass initializes at least one routine or finds
    // nothing left, and a routine's initialization may register further routines for the next pass.
    while (true)
    {
        auto const pass = tick();
        total.initialized += pass.initialized;
        total.pending = pass.pending;
        if (pass.initialized == 0)
            break;
    }
    return total;
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

routine_readiness routine_registry::readiness_of(render_routine_base& routine)
{
    auto const own = routine.own_readiness();
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
    // Edges first: a token holds a strong reference, so a cycle that slipped past add_dependency would otherwise keep
    // its own routines alive after the map let go of them.
    _edges.lock([](edge_map& edges) { edges.clear(); });
    _entries.lock([](routine_map& entries) { entries.clear(); });
}
} // namespace sg
