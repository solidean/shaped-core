#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/transient.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/memory/allocation_info.hh>
#include <shaped-graphics/memory/memory_heap.hh>

namespace sg
{
void context_transient_scope::set_budget(isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes > 0, "transient budget must be positive");
    // Record the request; it is applied at the next advance_epoch (see apply_pending_budget_at_epoch_boundary).
    _bump.lock([&](bump_state& s) { s.pending_budget = size_in_bytes; });
}

void context_transient_scope::apply_pending_budget_at_epoch_boundary()
{
    // Never waits: the heap is only dropped here, and the next allocation creates one at the new budget.
    // Every transient resource placed in the old heap holds a handle to it, and its release is epoch-deferred.
    // So the old heap lives exactly as long as the GPU work that still reads it.
    _bump.lock(
        [&](bump_state& s)
        {
            if (s.pending_budget == 0)
                return;
            s.heap = nullptr;
            s.budget = s.pending_budget;
            s.head = 0;
            s.pending_budget = 0;
        });
}

raw_buffer_handle context_transient_scope::create_raw_buffer(isize size_in_bytes, buffer_usages usage)
{
    auto r = try_create_raw_buffer(size_in_bytes, usage);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("transient buffer allocation failed", size_in_bytes, r.error());
}

cc::result<raw_buffer_handle> context_transient_scope::try_create_raw_buffer(isize size_in_bytes, buffer_usages usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    allocation_info alloc;
    alloc.scope = lifetime_scope::transient;

    // Empty buffers own no storage; a dedicated transient allocation is enough.
    // Non-empty ones reserve a window from the per-epoch bump allocator, resetting the head when the epoch changes.
    if (size_in_bytes > 0)
    {
        auto reserved = _bump.lock(
            [&](bump_state& s) -> cc::result<allocation_info>
            {
                if (!s.heap) // lazily create the backing heap on first use
                {
                    auto heap = _ctx.try_create_memory_heap(s.budget);
                    CC_RETURN_IF_ERROR(heap);
                    s.heap = heap.value();
                    s.budget = s.heap->size_in_bytes();
                }

                u64 const epoch_now = u64(_ctx.current_epoch());
                if (epoch_now != s.last_epoch) // new epoch: reset the head (aliases prior epochs' storage)
                {
                    s.head = 0;
                    s.last_epoch = epoch_now;
                }

                // The head sits wherever the previous allocation ended, which a usage with a stricter alignment may not accept.
                // So each allocation starts at the head rounded up to its own alignment, never at the head itself.
                memory_requirements const reqs = s.heap->memory_requirements_for_buffer(size_in_bytes, usage);
                auto const offset = cc::int_round_up_to_multiple(s.head, reqs.alignment_in_bytes);
                if (offset + reqs.size_in_bytes > s.budget)
                    return allocation_info{.scope = lifetime_scope::transient}; // over budget: committed fallback

                allocation_info a = s.heap->acquire_allocation_for_buffer(size_in_bytes, usage, offset);
                a.scope = lifetime_scope::transient;
                s.head = offset + reqs.size_in_bytes;
                return a;
            });
        CC_RETURN_IF_ERROR(reserved);
        alloc = reserved.value();
    }

    return _ctx.try_create_raw_buffer(size_in_bytes, usage, alloc);
}

raw_texture_handle context_transient_scope::create_raw_texture(texture_description const& desc)
{
    auto r = try_create_raw_texture(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("transient texture allocation failed", 0, r.error());
}

cc::result<raw_texture_handle> context_transient_scope::try_create_raw_texture(texture_description const& desc)
{
    // WORKAROUND: the transient bump-heap is buffers-only, so a transient texture is a dedicated allocation tagged transient, which the backend auto-expires at the next epoch.
    // Placed/bump-allocated transient textures wait on a texture-capable transient memory_heap; see the header note.
    allocation_info alloc;
    alloc.scope = lifetime_scope::transient;
    return _ctx.try_create_raw_texture(desc, alloc);
}

binding_group_handle context_transient_scope::create_binding_group(binding_group_layout_handle layout,
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

cc::result<binding_group_handle> context_transient_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                   cc::span<named_view const> views,
                                                                                   cc::span<named_sampler const> samplers)
{
    return _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::transient);
}

binding_group_handle context_transient_scope::create_binding_group(binding_group_layout_handle layout,
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

cc::result<binding_group_handle> context_transient_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                   cc::span<slotted_view const> views,
                                                                                   cc::span<named_sampler const> samplers)
{
    return _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::transient);
}
} // namespace sg

namespace sg
{
void context_transient_scope::release_heap_at_shutdown()
{
    // Nothing is drained here: shutdown has already waited the device idle, and a backend that has not is broken in
    // ways this cannot fix.
    _bump.lock(
        [](bump_state& s)
        {
            s.heap = nullptr;
            s.head = 0;
            s.last_epoch = 0;
        });
}
} // namespace sg

sg::raw_view sg::context_transient_scope::implicit_constants(cc::vector<byte> block)
{
    // A uniform block is read in rows of 16 bytes, and a buffer holding one is sized in 256-byte units on dx12.
    auto const view_size = cc::align_up(block.size(), isize(16));
    auto const raw = create_raw_buffer(cc::align_up(view_size, uniform_buffer_offset_alignment),
                                       buffer_usage::uniform_buffer | buffer_usage::copy_dst);
    _ctx.upload.bytes_to_buffer(raw, cc::make_pinned_data(cc::move(block)));
    return raw_buffer_view{.access = view_class::uniform,
                           .shape = view_shape::uniform_block,
                           .buffer = raw,
                           .offset_in_bytes = 0,
                           .size_in_bytes = view_size};
}
