#include "allocation.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <mimalloc.h>

using namespace cc::primitive_defines;

// mimalloc-backed implementation of cc::default_memory_resource.
// The <mimalloc.h> dependency is confined to this translation unit, so clean-core's headers stay free of it.

namespace
{
isize mi_try_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
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
    *out_ptr = static_cast<byte*>(p);
    if (p == nullptr)
    {
        return -1;
    }

    // Report mimalloc's actual usable size (size-class rounding), clamped into
    // [min_bytes, max_bytes] so containers can claim the extra capacity for free.
    isize const usable = static_cast<isize>(mi_usable_size(p));
    return usable < max_bytes ? usable : max_bytes;
}

isize mi_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    auto const result = mi_try_allocate_bytes(out_ptr, min_bytes, max_bytes, alignment, userdata);
    CC_ASSERTF(min_bytes == 0 || result >= 0, "allocation failed: requested [{}, {}] bytes with alignment {}",
               min_bytes, max_bytes, alignment);
    return result;
}

void mi_deallocate_bytes(byte* p, isize bytes, isize alignment, void* userdata)
{
    CC_UNUSED(bytes);
    CC_UNUSED(alignment);
    CC_UNUSED(userdata);

    // mimalloc tracks the block's size and alignment internally, so a plain mi_free suffices.
    mi_free(p);
}

isize mi_try_resize_bytes_in_place(byte* p, isize old_bytes, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    CC_UNUSED(old_bytes);
    CC_UNUSED(userdata);

    CC_ASSERT(p != nullptr, "cannot resize null pointer");
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(old_bytes > 0, "old_bytes must be positive");
    CC_ASSERT(1 <= min_bytes && min_bytes <= max_bytes, "must have 1 <= min_bytes <= max_bytes");

    // An in-place resize succeeds iff min_bytes still fits the block's usable size, and the block never moves, so pointers into the allocation stay valid.
    // mi_usable_size rather than mi_expand: mi_expand is a documented no-op under mimalloc's padding/guard debug modes (MI_DEBUG / MI_SECURE), which would make every resize fail there.
    // mi_usable_size excludes the padding region, so reporting capacity up to it keeps writes inside the user block and leaves mimalloc's overflow canary intact.
    isize const usable = static_cast<isize>(mi_usable_size(p));
    if (min_bytes > usable)
    {
        return -1;
    }
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
