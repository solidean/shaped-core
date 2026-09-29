#include <clean-core/common/assert.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/compute.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

namespace sg
{
namespace
{
[[nodiscard]] int ceil_div(int a, int b)
{
    return b <= 0 ? a : (a + b - 1) / b;
}
} // namespace

void command_list::bind_compute_pipeline(compute_pipeline const& pipeline)
{
    if (auto const* layout = pipeline.footprint().layout(); layout == nullptr || layout != _compute_layout)
    {
        _compute_layout = layout;
        for (auto& g : _compute_groups)
            g = nullptr;
    }
    compute_bind_pipeline(pipeline);
}

void command_list::bind_compute_group(int group_index, binding_group const& group)
{
    CC_ASSERT(group_index >= 0 && group_index < max_binding_groups, "binding-group slot out of range");
    _compute_groups[group_index] = &group;
    compute_bind_group(group_index, group);
}

void command_list::dispatch(int x, int y, int z)
{
    if (context().portability_checks())
    {
        _compute_aliasing.clear();
        _compute_aliasing.add(_compute_groups, {}, nullptr);
    }
    compute_dispatch(x, y, z);
}

void command_list_compute_scope::bind_pipeline(compute_pipeline const& pipeline)
{
    compute_dimensions const wg = pipeline.workgroup_size();
    _bound_wg_x = wg.x;
    _bound_wg_y = wg.y;
    _bound_wg_z = wg.z;
    _cmd.bind_compute_pipeline(pipeline);
}

void command_list_compute_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_compute_group(group_index, group);
}

void command_list_compute_scope::dispatch_groups(int x, int y, int z)
{
    _cmd._stats.add(stat::dispatches);
    _cmd.dispatch(x, y, z);
}

void command_list_compute_scope::dispatch_threads(int x, int y, int z)
{
    _cmd._stats.add(stat::dispatches);
    _cmd.dispatch(ceil_div(x, _bound_wg_x), ceil_div(y, _bound_wg_y), ceil_div(z, _bound_wg_z));
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

void command_list_compute_scope::set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    _cmd.compute_set_inline_constants(data, offset);
}
} // namespace sg
