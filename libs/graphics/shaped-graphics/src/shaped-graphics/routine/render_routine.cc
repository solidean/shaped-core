#include <clean-core/common/profiling.hh>
#include <shaped-graphics/command_list/command_list.hh> // cmd.context()
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-graphics/routine/render_routine_base.hh>

namespace sg
{
void render_routine_base::ensure_initialized_no_materialize_impl(init_state& s, context& ctx)
{
    if (!s.once_done)
    {
        init_once(ctx);
        s.once_done = true;
    }

    u64 const current = current_generation();
    if (s.declared_generation != current)
    {
        // A fresh generation is a fresh verdict: whatever failed last time is given another chance to compile.
        _failed = false;
        // Re-runs on every shader reload, so this is what a hot-reload hitch costs.
        CC_RECORD_SCOPE("sg.routine.declare");
        init_declare(ctx);
        s.declared_generation = current;
    }
}

void render_routine_base::ensure_initialized_impl(init_state& s, command_list& cmd)
{
    ensure_initialized_no_materialize_impl(s, cmd.context());

    // Compared against the generation declare just ran at, not a fresh read of the counter:
    // a reload landing between the two would otherwise leave materialize marked current against a stale declare,
    // and the next frame would re-run declare alone and never catch materialize up.
    if (s.materialized_generation != s.declared_generation)
    {
        // Where a routine's pipelines are actually created, so the multi-millisecond half of a reload lands here.
        CC_RECORD_SCOPE("sg.routine.materialize");
        init_materialize(cmd);
        s.materialized_generation = s.declared_generation;
    }
}

void render_routine_base::ensure_initialized_no_materialize(context& ctx)
{
    // The lock is held across the phase callbacks on purpose:
    // a second thread acquiring the same routine must wait and then observe it initialized, rather than run init_declare a second time in parallel.
    _init.lock([&](init_state& s) { ensure_initialized_no_materialize_impl(s, ctx); });
}

void render_routine_base::ensure_initialized(command_list& cmd)
{
    _init.lock([&](init_state& s) { ensure_initialized_impl(s, cmd); });
}

bool render_routine_base::is_initialized()
{
    auto const current = current_generation();
    return _init.lock(
        [current](init_state& s)
        { return s.once_done && s.declared_generation == current && s.materialized_generation == current; });
}

routine_readiness render_routine_base::own_readiness_locked(init_state const& s)
{
    auto const current = current_generation();
    auto const done = s.once_done && s.declared_generation == current && s.materialized_generation == current;
    if (!done)
        return routine_readiness::pending;
    return _failed ? routine_readiness::failed : routine_readiness::ready;
}

void render_routine_base::fail_init()
{
    // Called from inside a phase, which already runs under _init — so the flag is written directly rather than by
    // re-taking a lock cc::mutex would not let us take twice.
    _failed = true;
}

routine_readiness render_routine_base::own_readiness()
{
    return _init.lock([this](init_state& s) { return own_readiness_locked(s); });
}

routine_readiness render_routine_base::readiness()
{
    return own_readiness();
}

u64 render_routine_base::current_generation()
{
    return sg::reload_generation();
}
} // namespace sg
