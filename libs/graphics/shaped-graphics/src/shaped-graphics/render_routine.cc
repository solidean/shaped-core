#include <shaped-graphics/command_list.hh> // cmd.context()
#include <shaped-graphics/reload_generation.hh>
#include <shaped-graphics/render_routine_base.hh>

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
        init_declare(ctx);
        s.declared_generation = current;
    }
}

void render_routine_base::ensure_initialized_impl(init_state& s, command_list& cmd)
{
    ensure_initialized_no_materialize_impl(s, cmd.context());

    // Compared against the generation declare just ran at, not a fresh read of the counter: a reload
    // landing between the two would otherwise leave materialize marked current against a stale declare,
    // and the next frame would re-run declare alone and never catch materialize up.
    if (s.materialized_generation != s.declared_generation)
    {
        init_materialize(cmd);
        s.materialized_generation = s.declared_generation;
    }
}

void render_routine_base::ensure_initialized_no_materialize(context& ctx)
{
    // The lock is held across the phase callbacks on purpose: a second thread acquiring the same routine
    // must wait and then observe it initialized, rather than run init_declare a second time in parallel.
    _init.lock([&](init_state& s) { ensure_initialized_no_materialize_impl(s, ctx); });
}

void render_routine_base::ensure_initialized(command_list& cmd)
{
    _init.lock([&](init_state& s) { ensure_initialized_impl(s, cmd); });
}

u64 render_routine_base::current_generation()
{
    return sg::reload_generation();
}
} // namespace sg
