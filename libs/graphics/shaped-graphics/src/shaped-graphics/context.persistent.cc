#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.persistent.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg
{
// buffers

raw_buffer_handle context_persistent_scope::create_raw_buffer(isize size_in_bytes,
                                                              buffer_usage usage,
                                                              allocation_info const& alloc)
{
    auto r = try_create_raw_buffer(size_in_bytes, usage, alloc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("persistent buffer allocation failed", size_in_bytes, r.error());
}

cc::result<raw_buffer_handle> context_persistent_scope::try_create_raw_buffer(isize size_in_bytes,
                                                                              buffer_usage usage,
                                                                              allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.try_create_raw_buffer(size_in_bytes, usage, alloc);
}

// textures

raw_texture_handle context_persistent_scope::create_raw_texture(texture_description const& desc,
                                                                allocation_info const& alloc)
{
    auto r = try_create_raw_texture(desc, alloc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("persistent texture allocation failed", 0, r.error());
}

cc::result<raw_texture_handle> context_persistent_scope::try_create_raw_texture(texture_description const& desc,
                                                                                allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.try_create_raw_texture(desc, alloc);
}

// memory heaps

memory_heap_handle context_persistent_scope::create_memory_heap(isize size_in_bytes)
{
    auto r = try_create_memory_heap(size_in_bytes);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("memory heap allocation failed", size_in_bytes, r.error());
}

cc::result<memory_heap_handle> context_persistent_scope::try_create_memory_heap(isize size_in_bytes)
{
    return _ctx.try_create_memory_heap(size_in_bytes);
}

// bind path
// binding_layout / compute_pipeline creation lives on ctx.uncached (see context.uncached.cc) — they are
// schemas / PSOs, not lifetime-scoped resources. binding_group is a real per-scope descriptor allocation.

binding_group_handle context_persistent_scope::create_binding_group(binding_layout_handle layout,
                                                                    cc::span<named_view const> views,
                                                                    cc::span<named_sampler const> samplers)
{
    auto r = try_create_binding_group(cc::move(layout), views, samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<binding_group_handle> context_persistent_scope::try_create_binding_group(binding_layout_handle layout,
                                                                                    cc::span<named_view const> views,
                                                                                    cc::span<named_sampler const> samplers)
{
    return _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::persistent);
}
} // namespace sg
