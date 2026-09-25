#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/impl/portability.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/persistent.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg
{
// buffers

raw_buffer_handle context_persistent_scope::create_raw_buffer(isize size_in_bytes,
                                                              buffer_usages usage,
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
                                                                              buffer_usages usage,
                                                                              allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.try_create_raw_buffer(size_in_bytes, usage, alloc);
}

raw_buffer_handle context_persistent_scope::create_raw_buffer_from_pin(cc::pinned_data<byte const> bytes,
                                                                       buffer_usages usage,
                                                                       allocation_info const& alloc)
{
    auto buffer = create_raw_buffer(bytes.size(), usage | buffer_usage::copy_dst, alloc);
    _ctx.upload.bytes_to_buffer(buffer, cc::move(bytes));
    return buffer;
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
    if (auto unsupported = impl::find_unsupported_texture(_ctx.supports(feature::extended_image_formats), desc);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    if (auto error = desc.unaligned_block_error(_ctx.supports(feature::unaligned_block_compression)); !error.empty())
        return cc::error(cc::move(error));
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
// binding_group_layout / pipeline_layout / compute_pipeline creation lives on ctx.uncached (see uncached.cc) — those are schemas / PSOs, not lifetime-scoped resources.
// binding_group is a real per-scope descriptor allocation.

binding_group_handle context_persistent_scope::create_binding_group(binding_group_layout_handle layout,
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

cc::result<binding_group_handle> context_persistent_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                    cc::span<named_view const> views,
                                                                                    cc::span<named_sampler const> samplers)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    if (auto unsupported
        = impl::find_unsupported_view(_ctx.supports(feature::float32_filtering), layout->bindings(), views);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    return _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::persistent);
}

binding_group_handle context_persistent_scope::create_binding_group(binding_group_layout_handle layout,
                                                                    cc::span<slotted_view const> views,
                                                                    cc::span<named_sampler const> samplers)
{
    auto r = try_create_binding_group(cc::move(layout), views, samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<binding_group_handle> context_persistent_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                    cc::span<slotted_view const> views,
                                                                                    cc::span<named_sampler const> samplers)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    if (auto unsupported
        = impl::find_unsupported_view(_ctx.supports(feature::float32_filtering), layout->bindings(), views);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    return _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::persistent);
}

staging_binding_group_handle context_persistent_scope::create_staging_binding_group(binding_group_layout_handle layout)
{
    auto r = try_create_staging_binding_group(cc::move(layout));
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<staging_binding_group_handle> context_persistent_scope::try_create_staging_binding_group(
    binding_group_layout_handle layout)
{
    auto created = _ctx.try_create_staging_binding_group(cc::move(layout), lifetime_scope::persistent);
    if (created.has_value())
        created.value()->_float32_filtering = _ctx.supports(feature::float32_filtering);
    return created;
}
} // namespace sg

sg::raw_view sg::context_persistent_scope::implicit_constants(cc::vector<byte> block)
{
    // A uniform block is read in rows of 16 bytes, and a buffer holding one is sized in 256-byte units on dx12.
    auto const view_size = cc::align_up(block.size(), isize(16));
    auto const raw = create_raw_buffer(cc::align_up(view_size, uniform_buffer_offset_alignment),
                                       buffer_usage::uniform_buffer | buffer_usage::copy_dst);
    _ctx.upload.bytes_to_buffer(raw, cc::make_pinned_data(cc::move(block)));
    return raw_buffer_view{.bound_as = view_class::uniform,
                           .shape = view_shape::uniform_block,
                           .buffer = raw,
                           .offset_in_bytes = 0,
                           .size_in_bytes = view_size};
}
