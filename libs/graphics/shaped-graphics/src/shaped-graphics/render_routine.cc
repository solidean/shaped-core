#include <shaped-graphics/command_list.hh> // cmd.context()
#include <shaped-graphics/reload_generation.hh>
#include <shaped-graphics/render_routine_base.hh>

namespace sg
{
void render_routine_base::ensure_initialized_no_materialize(context& ctx)
{
    if (!_init_once_done)
    {
        init_once(ctx);
        _init_once_done = true;
    }

    u64 const current = current_generation();
    if (_declared_generation != current)
    {
        init_declare(ctx);
        _declared_generation = current;
    }
}

void render_routine_base::ensure_initialized(command_list& cmd)
{
    ensure_initialized_no_materialize(cmd.context());

    u64 const current = current_generation();
    if (_materialized_generation != current)
    {
        init_materialize(cmd);
        _materialized_generation = current;
    }
}

u64 render_routine_base::current_generation()
{
    return sg::reload_generation();
}
} // namespace sg
