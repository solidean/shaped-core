#include "allocation.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <mimalloc.h>

// mimalloc-backed implementation of cc::default_memory_resource. mimalloc is a fast
// general-purpose allocator; routing the default resource through it speeds up every
// non-node allocation (cc::vector, cc::string, ...). The <mimalloc.h> dependency is
// confined to this translation unit so clean-core's headers stay free of it.
//
// The node allocator (node_allocation.*) is a separate fast path and is unaffected,
// though it now draws its slabs from mimalloc via this default resource.

namespace
{
cc::isize mi_try_allocate_bytes(cc::byte** out_ptr,
                                cc::isize min_bytes,
                                cc::isize max_bytes,
                                cc::isize alignment,
                                void* userdata)
{
    CC_UNUSED(userdata);

    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(min_bytes >= 0 && min_bytes <= max_bytes, "must have 0 <= min_bytes <= max_bytes");

    // Contract: min_bytes == 0 always sets *out_ptr to nullptr and returns 0
    if (min_bytes == 0)
    {
        *out_ptr = nullptr;
        return 0;
    }

    void* p = mi_malloc_aligned(static_cast<size_t>(min_bytes), static_cast<size_t>(alignment));
    *out_ptr = static_cast<cc::byte*>(p);
    if (p == nullptr)
    {
        return -1;
    }

    // Report mimalloc's actual usable size (size-class rounding), clamped into
    // [min_bytes, max_bytes] so containers can claim the extra capacity for free.
    cc::isize const usable = static_cast<cc::isize>(mi_usable_size(p));
    return usable < max_bytes ? usable : max_bytes;
}

cc::isize mi_allocate_bytes(cc::byte** out_ptr, cc::isize min_bytes, cc::isize max_bytes, cc::isize alignment, void* userdata)
{
    auto const result = mi_try_allocate_bytes(out_ptr, min_bytes, max_bytes, alignment, userdata);
    CC_ASSERTF(min_bytes == 0 || result >= 0, "allocation failed: requested [{}, {}] bytes with alignment {}",
               min_bytes, max_bytes, alignment);
    return result;
}

void mi_deallocate_bytes(cc::byte* p, cc::isize bytes, cc::isize alignment, void* userdata)
{
    CC_UNUSED(bytes);
    CC_UNUSED(alignment);
    CC_UNUSED(userdata);

    // mimalloc tracks the block's size and alignment internally, so a plain mi_free suffices.
    mi_free(p);
}

cc::isize mi_try_resize_bytes_in_place(cc::byte* p,
                                       cc::isize old_bytes,
                                       cc::isize min_bytes,
                                       cc::isize max_bytes,
                                       cc::isize alignment,
                                       void* userdata)
{
    CC_UNUSED(old_bytes);
    CC_UNUSED(userdata);

    CC_ASSERT(p != nullptr, "cannot resize null pointer");
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(old_bytes > 0, "old_bytes must be positive");
    CC_ASSERT(1 <= min_bytes && min_bytes <= max_bytes, "must have 1 <= min_bytes <= max_bytes");

    // mi_expand never moves the block: it returns p if min_bytes still fits the
    // block's size class, otherwise nullptr. This lets contiguous containers grow
    // (or shrink) without invalidating pointers into the allocation.
    if (mi_expand(p, static_cast<size_t>(min_bytes)) == nullptr)
    {
        return -1;
    }

    cc::isize const usable = static_cast<cc::isize>(mi_usable_size(p));
    return usable < max_bytes ? usable : max_bytes;
}

/// mimalloc memory resource instance, data-segment resident so the default-resource
/// pointer is valid during static initialization in other translation units.
constinit cc::memory_resource const mimalloc_memory_resource = {
    .allocate_bytes = mi_allocate_bytes,
    .try_allocate_bytes = mi_try_allocate_bytes,
    .deallocate_bytes = mi_deallocate_bytes,
    .try_resize_bytes_in_place = mi_try_resize_bytes_in_place,
    .userdata = nullptr,
};

} // namespace

constinit cc::memory_resource const* const cc::default_memory_resource = &mimalloc_memory_resource;
