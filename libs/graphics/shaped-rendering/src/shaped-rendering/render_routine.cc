#include <shaped-graphics/command_list.hh> // cmd.context()
#include <shaped-rendering/render_routine.hh>
#include <shaped-shader-library/shader_library.hh> // slib::current_reload_generation

namespace sr
{
void render_routine::ensure_initialized_no_materialize(sg::context& ctx)
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

void render_routine::ensure_initialized(sg::command_list& cmd)
{
    ensure_initialized_no_materialize(cmd.context());

    u64 const current = current_generation();
    if (_materialized_generation != current)
    {
        init_materialize(cmd);
        _materialized_generation = current;
    }
}

u64 render_routine::current_generation()
{
    return slib::current_reload_generation();
}
} // namespace sr
