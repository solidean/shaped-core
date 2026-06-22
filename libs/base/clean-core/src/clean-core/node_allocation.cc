#include "node_allocation.hh"

#include <clean-core/allocation.hh>
#include <clean-core/bit.hh>

#include <atomic>

namespace
{

// forward declaration
cc::byte* system_allocate_node_bytes_large(cc::node_class_index idx,
                                           cc::isize size_bytes,
                                           cc::isize alignment,
                                           void* userdata);
void system_deallocate_node_bytes_large(cc::byte* ptr, cc::node_class_index idx, void* userdata);
cc::byte* system_refill_slabs_and_allocate_node_bytes(cc::node_allocator::slab_info& slabs,
                                                      cc::node_class_index idx,
                                                      void* userdata);

// forward declaration of the system node memory resource (defined below)
extern cc::node_memory_resource system_node_memory_resource;

/// Returns a thread-local node_allocator for the system node memory resource.
/// The allocator is lazy-initialized with all slabs nullptr.
cc::node_allocator& system_get_allocator(void* userdata)
{
    CC_UNUSED(userdata);
    thread_local cc::node_allocator tls_allocator(&system_node_memory_resource);
    return tls_allocator;
}

/// Allocates a large node (> small_max) from the system memory resource.
/// Stores a 24-byte header: (size, alignment, resource pointer).
/// Alignment is bumped to at least 8 bytes to ensure proper alignment for the header.
cc::byte* system_allocate_node_bytes_large(cc::node_class_index idx, cc::isize size_bytes, cc::isize alignment, void* userdata)
{
    CC_UNUSED(idx); // not needed for system allocator
    CC_UNUSED(userdata);

    // bump alignment to at least 8 bytes to ensure proper storage of the header
    alignment = cc::max(alignment, cc::isize(8));
    CC_ASSERT(alignment == 8, "TODO: for larger alignments we need to allocate a bit better");

    // header layout: [size (8B)][alignment (8B)][resource* (8B)][actual allocation]
    constexpr cc::isize header_size = 24;
    cc::isize const total_size = header_size + size_bytes;

    // allocate from system memory resource
    cc::byte* alloc_ptr = nullptr;
    cc::isize const actual_size = cc::default_memory_resource->allocate_bytes(
        &alloc_ptr, total_size, total_size, alignment, cc::default_memory_resource->userdata);
    CC_ASSERT(actual_size >= total_size, "system allocator must allocate at least the requested size");
    CC_ASSERT(alloc_ptr != nullptr, "system allocator must return non-null for non-zero size");

    // write header: size, alignment, resource pointer
    *reinterpret_cast<cc::isize*>(alloc_ptr + 0) = size_bytes;                                    // NOLINT
    *reinterpret_cast<cc::isize*>(alloc_ptr + 8) = alignment;                                     // NOLINT
    *reinterpret_cast<cc::node_memory_resource**>(alloc_ptr + 16) = &system_node_memory_resource; // NOLINT

    // return pointer past the header
    return alloc_ptr + header_size;
}

/// Deallocates a large node (> small_max) by reading the header and calling into system memory resource.
void system_deallocate_node_bytes_large(cc::byte* ptr, cc::node_class_index idx, void* userdata)
{
    CC_UNUSED(idx); // not needed for system allocator
    CC_UNUSED(userdata);

    // header is 24 bytes before the user pointer
    constexpr cc::isize header_size = 24;
    cc::byte* const alloc_ptr = ptr - header_size;

    // read header: size, alignment, resource pointer
    cc::isize const size_bytes = *reinterpret_cast<cc::isize*>(alloc_ptr + 0);           // NOLINT
    cc::isize const alignment = *reinterpret_cast<cc::isize*>(alloc_ptr + 8);            // NOLINT
    auto const resource = *reinterpret_cast<cc::node_memory_resource**>(alloc_ptr + 16); // NOLINT

    CC_ASSERT(resource == &system_node_memory_resource, "resource mismatch in large node deallocation");

    // deallocate from system memory resource
    cc::isize const total_size = header_size + size_bytes;
    cc::default_memory_resource->deallocate_bytes(alloc_ptr, total_size, alignment,
                                                  cc::default_memory_resource->userdata);
}

/// Refills slabs for a given size class by allocating a new slab from the system memory resource.
/// Sets up the freemap with appropriate slots forced to zero (for the header metadata).
/// Wires the new slab into the cyclic slab list (making it the new head).
cc::byte* system_refill_slabs_and_allocate_node_bytes(cc::node_allocator::slab_info& slabs,
                                                      cc::node_class_index idx,
                                                      void* userdata)
{
    CC_UNUSED(userdata);
    // allocate a new slab from the system memory resource
    cc::isize const slab_size = cc::node_slab_size_bytes_for_class(idx);
    cc::isize const slab_alignment = slab_size; // slabs are aligned to their own size

    cc::byte* new_slab = nullptr;
    cc::isize const actual_size = cc::default_memory_resource->allocate_bytes(
        &new_slab, slab_size, slab_size, slab_alignment, cc::default_memory_resource->userdata);
    CC_ASSERT(actual_size >= slab_size, "system allocator must allocate at least the requested slab size");
    CC_ASSERT(new_slab != nullptr, "system allocator must return non-null for slab allocation");
    CC_ASSERT(cc::is_aligned(new_slab, slab_alignment), "slab must be aligned to its own size");

    // compute how many slots are blocked by the 16-byte header (freemap + next pointer)
    // header is 16 bytes: [freemap (8B)][next slab pointer (8B)]
    constexpr cc::isize header_bytes = 16;
    cc::isize const class_size = cc::isize(1) << cc::isize(idx);                  // 2^idx
    cc::isize const blocked_slots = (header_bytes + class_size - 1) / class_size; // round up division

    // initialize freemap: all bits set to 1 (free), except for blocked slots
    cc::u64 initial_freemap = ~cc::u64(0);                          // all 1s
    cc::u64 const blocked_mask = (cc::u64(1) << blocked_slots) - 1; // mask for blocked slots
    initial_freemap &= ~blocked_mask;                               // clear bits for blocked slots

    // write freemap
    *reinterpret_cast<cc::u64*>(new_slab) = initial_freemap; // NOLINT

    // get reference to the slab base for this class
    cc::byte*& slab_base_ref = slabs.slab_base[cc::isize(idx)];

    // DEBUG: for now we simply pass out new self-cycles
    // TODO: keep non-empty previous slabs in here as well
    // FIXME: this is currently basically a memory leak here, but it's fine for correctness for now
    *reinterpret_cast<cc::byte**>(new_slab + 8) = new_slab; // NOLINT

    // update slab base reference to point to the new slab (making it the new head)
    slab_base_ref = new_slab;

    // allocate the first free slot from the new slab
    auto const a_freemap = std::atomic_ref<cc::u64>(*cc::node_slab_freemap_for_base(new_slab));
    auto const freemap = a_freemap.load();
    CC_ASSERT(freemap != 0, "newly allocated slab must have at least one free slot");

    auto const slot_idx = cc::count_trailing_zeroes(freemap);
    auto const slot_bit = cc::u64(1) << slot_idx;
    a_freemap.fetch_and(~slot_bit);

    return cc::node_slot_ptr_for(new_slab, idx, slot_idx);
}

constinit cc::node_memory_resource system_node_memory_resource = {
    .get_allocator = system_get_allocator,
    .allocate_node_bytes_large = system_allocate_node_bytes_large,
    .refill_slabs_and_allocate_node_bytes = system_refill_slabs_and_allocate_node_bytes,
    .deallocate_node_bytes_large = system_deallocate_node_bytes_large,
    .userdata = nullptr,
};

} // namespace

constinit cc::node_memory_resource* const cc::default_node_memory_resource = &system_node_memory_resource;

void cc::node_allocation_free_large(cc::byte* ptr, node_class_index idx)
{
    CC_ASSERT(cc::is_aligned(ptr, 8), "large node allocations must be at least 8-byte aligned");

    // retrieve the resource pointer stored directly before the allocation
    auto const resource = *reinterpret_cast<cc::node_memory_resource**>(ptr - 8); // NOLINT
    CC_ASSERT(resource != nullptr, "resource pointer must be valid");
    CC_ASSERT(resource->deallocate_node_bytes_large != nullptr, "resource must implement deallocate_node_bytes_large");

    resource->deallocate_node_bytes_large(ptr, idx, resource->userdata);
}

cc::byte* cc::node_allocator::allocate_node_bytes_large(node_class_index idx, isize size_bytes, isize alignment)
{
    CC_ASSERT(_resource != nullptr, "node_allocator must have a valid resource");
    CC_ASSERT(_resource->allocate_node_bytes_large != nullptr, "resource must implement allocate_node_bytes_large");
    return _resource->allocate_node_bytes_large(idx, size_bytes, alignment, _resource->userdata);
}

cc::byte* cc::node_allocator::refill_slabs_and_allocate_node_bytes(node_class_index idx)
{
    CC_ASSERT(_resource != nullptr, "node_allocator must have a valid resource");
    CC_ASSERT(_resource->refill_slabs_and_allocate_node_bytes != nullptr, "resource must implement "
                                                                          "refill_slabs_and_allocate_node_bytes");
    return _resource->refill_slabs_and_allocate_node_bytes(_slabs, idx, _resource->userdata);
}

cc::byte* cc::node_allocator::allocate_node_bytes_non_fast(node_class_index idx)
{
    if (_slabs.slab_base[isize(idx)] == nullptr) [[unlikely]]
        return this->refill_slabs_and_allocate_node_bytes(idx);

    auto const start_base = _slabs.slab_base[isize(idx)];
    CC_ASSERT(start_base != nullptr, "node class should be initialized");

    // slab is initialized but full
    // we now iterate the slab ring to find a new free slab
    // this is still reasonably hot (it's the full node capacity for this thread without refill)
    // TODO: this is currently non-ideal for alloc-only workflows
    //       because we go through the list for each exhaustion
    //       with some minimal bookkeeping, we can keep this cheaper
    //       maybe slabs need to know the owning allocators after all
    auto base = cc::node_slab_next_for_base(start_base);
    while (base != start_base)
    {
        CC_ASSERT(base != nullptr, "the slab ring must be a cycling single-linked-list. indicates a "
                                   "node_memory_resource bug.");

        auto const a_freemap = std::atomic_ref<u64>(*cc::node_slab_freemap_for_base(base));
        auto const freemap = a_freemap.load();

        if (freemap != 0) [[likely]]
        {
            // record next free slab
            _slabs.slab_base[isize(idx)] = base;

            // allocate & return
            auto const slot_idx = cc::count_trailing_zeroes(freemap);
            auto const slot_bit = u64(1) << slot_idx;
            // updating the freemap must happen atomically
            // we're the only thread allocating from it
            // BUT there can be many threads that concurrently free
            auto const old_freemap = a_freemap.fetch_and(~slot_bit);
            CC_ASSERT((old_freemap & slot_bit) != 0, "double-allocation detected. this indicates multiple threads "
                                                     "allocating from the same slab");
            return cc::node_slot_ptr_for(base, idx, slot_idx);
        }

        // advance
        base = cc::node_slab_next_for_base(base);
    }

    // all slabs in the ring are full
    // => we request more from the allocator
    return this->refill_slabs_and_allocate_node_bytes(idx);
}

cc::node_allocator& cc::default_node_allocator()
{
    return cc::default_node_memory_resource->get_allocator(cc::default_node_memory_resource->userdata);
}
