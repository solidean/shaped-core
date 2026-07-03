#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.hh>

namespace sg
{
void command_list_compute_scope::bind_pipeline(compute_pipeline const& pipeline)
{
    _cmd.compute_bind_pipeline(pipeline);
}

void command_list_compute_scope::bind_group(u32 set, binding_group const& group)
{
    _cmd.compute_bind_group(set, group);
}

void command_list_compute_scope::dispatch(u32 x, u32 y, u32 z)
{
    _cmd.compute_dispatch(x, y, z);
}
} // namespace sg
