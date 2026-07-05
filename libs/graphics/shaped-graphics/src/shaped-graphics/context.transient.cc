#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.transient.hh>
#include <shaped-graphics/memory_heap.hh>

namespace sg
{
void context_transient_scope::set_buffer_budget(isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes > 0, "transient buffer budget must be positive");
    _bump.lock(
        [&](bump_state& s)
        {
            CC_ASSERT(s.heap == nullptr, "transient buffer budget must be set before the first transient buffer");
            s.budget = size_in_bytes;
        });
}

cc::result<buffer_handle> context_transient_scope::create_buffer(isize size_in_bytes, buffer_usage usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    allocation_info alloc;
    alloc.scope = lifetime_scope::transient;

    // Empty buffers own no storage; a dedicated transient allocation is enough. Non-empty ones reserve a
    // window from the per-epoch bump allocator, resetting the head when the epoch changes.
    if (size_in_bytes > 0)
    {
        auto reserved = _bump.lock(
            [&](bump_state& s) -> cc::result<allocation_info>
            {
                if (!s.heap) // lazily create the backing heap on first use
                {
                    auto heap = _ctx.create_memory_heap(s.budget);
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

                memory_requirements const reqs = s.heap->memory_requirements_for_buffer(size_in_bytes, usage);
                if (s.head + reqs.size_in_bytes > s.budget)
                    return allocation_info{.scope = lifetime_scope::transient}; // over budget: committed fallback

                allocation_info a = s.heap->acquire_allocation_for_buffer(size_in_bytes, usage, s.head);
                a.scope = lifetime_scope::transient;
                s.head += reqs.size_in_bytes;
                return a;
            });
        CC_RETURN_IF_ERROR(reserved);
        alloc = reserved.value();
    }

    return _ctx.create_buffer(size_in_bytes, usage, alloc);
}

cc::result<binding_group_handle> context_transient_scope::create_binding_group(binding_layout_handle layout,
                                                                               cc::span<named_view const> views)
{
    return _ctx.create_binding_group(cc::move(layout), views, lifetime_scope::transient);
}
} // namespace sg
