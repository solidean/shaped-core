#include "allocation.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>

#include <cstdlib>

using namespace cc::primitive_defines;

namespace
{
/// Static function implementations for the system memory resource.
/// These ignore the userdata parameter as the system allocator is stateless.

isize system_try_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
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

    // For simplicity, we allocate exactly min_bytes (not using max_bytes for size class rounding)
    // Custom allocators with size classes would use max_bytes to potentially round up
    isize bytes_to_allocate = min_bytes;

    // Use platform-specific aligned allocation:
    // - Windows: _aligned_malloc does not strictly require bytes % alignment == 0
    // - POSIX: posix_memalign does not require bytes % alignment == 0 (unlike std::aligned_alloc)
#ifdef CC_OS_WINDOWS
    *out_ptr = static_cast<byte*>(_aligned_malloc(bytes_to_allocate, alignment));
#else
    // Use posix_memalign instead of std::aligned_alloc to avoid the bytes % alignment == 0 requirement.
    // posix_memalign requires alignment >= sizeof(void*), so we clamp to that minimum.
    void* raw_ptr = nullptr;
    isize effective_alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    int result = posix_memalign(&raw_ptr, effective_alignment, bytes_to_allocate);
    *out_ptr = result == 0 ? static_cast<byte*>(raw_ptr) : nullptr;
#endif

    // Return actual allocated size, or -1 on failure
    return *out_ptr != nullptr ? bytes_to_allocate : -1;
}

isize system_allocate_bytes(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    auto const result = system_try_allocate_bytes(out_ptr, min_bytes, max_bytes, alignment, userdata);
    CC_ASSERTF(min_bytes == 0 || result >= 0, "allocation failed: requested [{}, {}] bytes with alignment {}",
               min_bytes, max_bytes, alignment);
    return result;
}

void system_deallocate_bytes(byte* p, isize bytes, isize alignment, void* userdata)
{
    CC_UNUSED(bytes);
    CC_UNUSED(alignment);
    CC_UNUSED(userdata);

    // Contract: size and alignment are provided for resources that need them (e.g., pooling),
    //           but standard malloc/free don't require them for deallocation.
    // IMPORTANT: Must use matching free function for the platform's allocator:
    // - Windows: _aligned_malloc requires _aligned_free (not free())
    // - POSIX: std::aligned_alloc uses std::free (not a special free function)
#ifdef CC_OS_WINDOWS
    _aligned_free(p);
#else
    std::free(p);
#endif
}

isize system_try_resize_bytes_in_place(byte* p, isize old_bytes, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
{
    CC_UNUSED(userdata);

    CC_ASSERT(p != nullptr, "cannot resize null pointer");
    CC_ASSERT(alignment > 0 && cc::is_power_of_two(alignment), "alignment must be a power of 2");
    CC_ASSERT(old_bytes > 0, "old_bytes must be positive");
    CC_ASSERT(1 <= min_bytes && min_bytes <= max_bytes, "must have 1 <= min_bytes <= max_bytes");

    CC_UNUSED(p);
    CC_UNUSED(old_bytes);
    CC_UNUSED(min_bytes);
    CC_UNUSED(max_bytes);
    CC_UNUSED(alignment);

    // Standard malloc/aligned_alloc do not support in-place resize.
    // Return -1 to signal failure (contract: allocation remains valid at p with size old_bytes).
    // Not realloc: moving the block would invalidate pointers into it — see try_resize_bytes_in_place in allocation.hh.
    return -1;
}

} // namespace
/// Platform malloc/free resource, stored in the data segment so it stays valid during static initialization.
/// The explicit opt-out from the mimalloc-backed default in mimalloc_resource.cc, reachable as a custom resource.
constinit cc::memory_resource const cc::system_memory_resource = {
    .allocate_bytes = system_allocate_bytes,
    .try_allocate_bytes = system_try_allocate_bytes,
    .deallocate_bytes = system_deallocate_bytes,
    .try_resize_bytes_in_place = system_try_resize_bytes_in_place,
    .userdata = nullptr,
};

#if !CC_HAS_MIMALLOC
/// Without mimalloc in the build the default resource IS the system one, since mimalloc_resource.cc — which otherwise
/// owns this definition — is not compiled at all.
/// So the opt-out and the default are the same object here, and code that compares the two must ask CC_HAS_MIMALLOC.
constinit cc::memory_resource const* const cc::default_memory_resource = &cc::system_memory_resource;
#endif
