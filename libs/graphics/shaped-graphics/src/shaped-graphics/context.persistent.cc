#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.persistent.hh>

namespace sg
{
cc::result<buffer_handle> context_persistent_scope::create_buffer(isize size_in_bytes,
                                                                  buffer_usage usage,
                                                                  allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.create_buffer(size_in_bytes, usage, alloc);
}

cc::result<memory_heap_handle> context_persistent_scope::create_memory_heap(isize size_in_bytes)
{
    return _ctx.create_memory_heap(size_in_bytes);
}

cc::result<binding_layout_handle> context_persistent_scope::create_binding_layout(cc::span<binding const> bindings)
{
    return _ctx.create_binding_layout(bindings, lifetime_scope::persistent);
}

cc::result<compute_pipeline_handle> context_persistent_scope::create_compute_pipeline(compute_pipeline_description const& desc)
{
    return _ctx.create_compute_pipeline(desc, lifetime_scope::persistent);
}

cc::result<binding_group_handle> context_persistent_scope::create_binding_group(binding_layout_handle layout,
                                                                                cc::span<named_view const> views)
{
    return _ctx.create_binding_group(cc::move(layout), views, lifetime_scope::persistent);
}
} // namespace sg
