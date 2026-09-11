#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
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

    auto result = routine_tick_result();

    auto pending = snapshot();
    if (pending.empty())
        return result;

    auto const start = cc::current_time_steady_secs();

    // One list for every routine this tick brings up, so their GPU init work batches into a single submit rather than
    // one per routine.
    // Opened lazily: a tick that finds nothing to materialize must not cost a command list.
    std::unique_ptr<command_list> cmd;

    for (auto const& routine : pending)
    {
        if (routine->is_initialized())
            continue;

        // Checked BETWEEN routines, never inside one — a routine's initialization is not interruptible, which is why
        // the budget is documented as pacing rather than a deadline.
        if (options.budget_secs.has_value() && cc::current_time_steady_secs() - start >= options.budget_secs.value())
        {
            result.budget_exhausted = true;
            break;
        }

        if (cmd == nullptr)
            cmd = _ctx.create_command_list();

        routine->ensure_initialized(*cmd);
        ++result.initialized;
    }

    if (cmd != nullptr)
        (void)_ctx.submit_command_list(cc::move(cmd));

    for (auto const& routine : pending)
        if (!routine->is_initialized())
            ++result.pending;

    return result;
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

void routine_registry::clear()
{
    _entries.lock([](routine_map& entries) { entries.clear(); });
}
} // namespace sg
