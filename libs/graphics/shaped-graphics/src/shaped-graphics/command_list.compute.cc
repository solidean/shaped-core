#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/compute_pipeline.hh>

namespace sg
{
namespace
{
[[nodiscard]] int ceil_div(int a, int b)
{
    return b <= 0 ? a : (a + b - 1) / b;
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

void command_list_compute_scope::bind_group(int set, binding_group const& group)
{
    _cmd.compute_bind_group(set, group);
}

void command_list_compute_scope::dispatch_groups(int x, int y, int z)
{
    _cmd.compute_dispatch(x, y, z);
}

void command_list_compute_scope::dispatch_threads(int x, int y, int z)
{
    _cmd.compute_dispatch(ceil_div(x, _bound_wg_x), ceil_div(y, _bound_wg_y), ceil_div(z, _bound_wg_z));
}

void command_list_compute_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                             cc::span<array_buffer_access const> elements)
{
    _cmd.compute_declare_array_buffer_access(binding_name, elements);
}

void command_list_compute_scope::declare_array_texture_access(cc::string_view binding_name,
                                                              cc::span<array_texture_access const> elements)
{
    _cmd.compute_declare_array_texture_access(binding_name, elements);
}

void command_list_compute_scope::set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    _cmd.compute_set_inline_constants(data, offset);
}
} // namespace sg
