#include <clean-core/common/assert.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/compute.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/buffer.hh>

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
    check_cluster_pools(_compute_groups, "dispatch");
    if (context().portability_checks())
    {
        _compute_aliasing.clear();
        _compute_aliasing.add(_compute_groups, {}, nullptr);
    }
    compute_dispatch(x, y, z);
}

void command_list::dispatch_indirect(raw_buffer_handle const& args, isize offset_in_bytes)
{
    CC_ASSERT(args != nullptr, "indirect dispatch arguments buffer is null");
    CC_ASSERT(args->usage().has(buffer_usage::indirect_command_buffer), "indirect dispatch arguments need "
                                                                        "buffer_usage::indirect_command_buffer");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes % 4 == 0, "indirect dispatch arguments offset must be a "
                                                                "non-negative multiple of 4");
    CC_ASSERT(offset_in_bytes + isize(sizeof(dispatch_args)) <= args->size_in_bytes(), "indirect dispatch arguments "
                                                                                       "extend past the buffer");
    check_cluster_pools(_compute_groups, "dispatch_indirect");
    if (context().portability_checks())
    {
        _compute_aliasing.clear();
        _compute_aliasing.add(_compute_groups, {}, nullptr);
        _compute_aliasing.add_indirect_args(args.get());
    }
    compute_dispatch_indirect(args, offset_in_bytes);
}

void command_list_compute_scope::bind_pipeline(compute_pipeline const& pipeline)
{
    compute_dimensions const wg = pipeline.workgroup_size();
    _bound_wg_x = wg.x;
    _bound_wg_y = wg.y;
    _bound_wg_z = wg.z;
    _bound_by_threads = pipeline.is_dispatched_by_threads();
    _cmd.bind_compute_pipeline(pipeline);
}

void command_list_compute_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_compute_group(group_index, group);
}

void command_list_compute_scope::dispatch_groups(int x, int y, int z)
{
    CC_ASSERT(!_bound_by_threads,
              "the bound pipeline's workgroup size is its compiler's choice, so it is dispatched by "
              "threads alone: dispatch_threads");
    _cmd._stats.add(stat::dispatches);
    _cmd.dispatch(x, y, z);
}

void command_list_compute_scope::dispatch_threads(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch thread counts must be non-negative");
    _cmd._stats.add(stat::dispatches);
    _cmd._dispatch_grid[0] = x;
    _cmd._dispatch_grid[1] = y;
    _cmd._dispatch_grid[2] = z;
    _cmd.dispatch(ceil_div(x, _bound_wg_x), ceil_div(y, _bound_wg_y), ceil_div(z, _bound_wg_z));
    _cmd._dispatch_grid[0] = 0;
    _cmd._dispatch_grid[1] = 0;
    _cmd._dispatch_grid[2] = 0;
}

void command_list_compute_scope::dispatch_groups_indirect(buffer<dispatch_args> const& args, isize index)
{
    CC_ASSERT(index >= 0 && index < args.element_count(), "indirect dispatch arguments index is out of range");
    dispatch_groups_indirect_bytes(args.raw(), index * isize(sizeof(dispatch_args)));
}

void command_list_compute_scope::dispatch_groups_indirect_bytes(raw_buffer_handle const& args, isize offset_in_bytes)
{
    CC_ASSERT(!_bound_by_threads,
              "the bound pipeline's workgroup size is its compiler's choice, so it is dispatched by "
              "threads alone: dispatch_threads");
    _cmd._stats.add(stat::dispatches);
    _cmd.dispatch_indirect(args, offset_in_bytes);
}

array_access_declaration command_list_compute_scope::declare_array_buffer_access(buffer_array_binding array,
                                                                                 cc::span<array_buffer_access const> elements)
{
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_buffer_access(array.name, elements), false);
}

array_access_declaration command_list_compute_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                                                 cc::span<array_buffer_access const> elements)
{
    return declare_array_buffer_access(_cmd.named_buffer_array(binding_name), elements);
}

array_access_declaration command_list_compute_scope::declare_array_texture_access(texture_array_binding array,
                                                                                  cc::span<array_texture_access const> elements)
{
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_texture_access(array.name, elements), false);
}

array_access_declaration command_list_compute_scope::declare_array_texture_access(cc::string_view binding_name,
                                                                                  cc::span<array_texture_access const> elements)
{
    return declare_array_texture_access(_cmd.named_texture_array(binding_name), elements);
}

void command_list_compute_scope::set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    _cmd.compute_set_inline_constants(data, offset);
}
} // namespace sg
