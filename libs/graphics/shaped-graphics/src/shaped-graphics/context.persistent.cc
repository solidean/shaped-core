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
    CC_ASSERT(alloc.scope == allocation_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.create_buffer(size_in_bytes, usage, alloc);
}

cc::result<binding_layout_handle> context_persistent_scope::create_binding_layout(cc::span<binding const> bindings)
{
    return _ctx.create_binding_layout(bindings);
}

cc::result<compute_pipeline_handle> context_persistent_scope::create_compute_pipeline(compiled_shader const& shader,
                                                                                      binding_layout_handle layout)
{
    return _ctx.create_compute_pipeline(shader, cc::move(layout));
}

cc::result<binding_group_handle> context_persistent_scope::create_binding_group(binding_layout_handle layout,
                                                                                cc::span<named_view const> views)
{
    return _ctx.create_binding_group(cc::move(layout), views);
}
} // namespace sg
