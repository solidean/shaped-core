#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/compute_pipeline.hh>

namespace sg
{
namespace
{
[[nodiscard]] u32 ceil_div(u32 a, u32 b)
{
    return b == 0 ? a : (a + b - 1) / b;
}
} // namespace

void command_list_compute_scope::bind_pipeline(compute_pipeline const& pipeline)
{
    compute_dimensions const wg = pipeline.workgroup_size();
    _bound_wg_x = wg.x;
    _bound_wg_y = wg.y;
    _bound_wg_z = wg.z;
    _cmd.compute_bind_pipeline(pipeline);
}

void command_list_compute_scope::bind_group(u32 set, binding_group const& group)
{
    _cmd.compute_bind_group(set, group);
}

void command_list_compute_scope::dispatch_groups(u32 x, u32 y, u32 z)
{
    _cmd.compute_dispatch(x, y, z);
}

void command_list_compute_scope::dispatch_threads(u32 x, u32 y, u32 z)
{
    _cmd.compute_dispatch(ceil_div(x, _bound_wg_x), ceil_div(y, _bound_wg_y), ceil_div(z, _bound_wg_z));
}
} // namespace sg
