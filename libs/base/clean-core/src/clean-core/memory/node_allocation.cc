#include "node_allocation.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>

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
/// Initializes the metadata (local freemap from the consteval seed mask; threaded: remote=0, owner stamped),
/// splices the new slab into the ring as the new head (retaining previous slabs), and allocates one slot.
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

    // initialize metadata: local freemap seeds the free slots (blocking the ones the metadata overlaps)
    cc::u64 const initial_freemap = cc::node_seed_local_freemaps[cc::isize(idx)];
    *cc::node_slab_freemap_for_base(new_slab) = initial_freemap;
#if CC_HAS_THREADS
    *cc::node_slab_remote_for_base(new_slab, idx) = 0;                // no remote frees yet
    *cc::node_slab_owner_for_base(new_slab) = cc::node_owner_token(); // stamp the hydrating thread
#endif

    // splice into the ring as the new head, retaining any previous slabs (they may hold free slots)
    cc::byte*& head = slabs.slab_base[cc::isize(idx)];
    if (head == nullptr) // first slab for this class: a self-cycle
        *cc::node_slab_next_ptr_for_base(new_slab) = new_slab;
    else // insert after the old head so the whole ring stays reachable
    {
        *cc::node_slab_next_ptr_for_base(new_slab) = cc::node_slab_next_for_base(head);
        *cc::node_slab_next_ptr_for_base(head) = new_slab;
    }
    head = new_slab;

    // allocate the first free slot from the new slab (owner-only, non-atomic — we just created it)
    CC_ASSERT(initial_freemap != 0, "newly allocated slab must have at least one free slot");
    auto const slot_idx = cc::count_trailing_zeroes(initial_freemap);
    *cc::node_slab_freemap_for_base(new_slab) = initial_freemap & ~(cc::u64(1) << slot_idx);

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

#if CC_HAS_THREADS
cc::u32 cc::detail::node_next_owner_id()
{
    // process-unique, never recycled: an id is never reused, so a free is never miscategorized.
    // ids are not reclaimed on thread exit (that leaks the thread's slabs -- a known, deferred follow-up).
    static std::atomic<cc::u32> s_next_owner_id{1}; // 0 is reserved for "unassigned"
    cc::u32 const id = s_next_owner_id.fetch_add(1, std::memory_order_relaxed);
    CC_ASSERT(id != 0, "node owner-id space exhausted (>4B threads ever); cross-thread-free after "
                       "thread-exit is unsupported");
    return id;
}
#endif

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
    auto const start_base = _slabs.slab_base[isize(idx)];
    if (start_base == nullptr) [[unlikely]]
        return this->refill_slabs_and_allocate_node_bytes(idx);

    // The head's local just emptied (that's why the fast path fell through), but any slab in the ring --
    // including the head -- may have cross-thread frees waiting in its remote bitmap. Walk the whole ring
    // from the head; on each slab with an empty local, drain remote into local (atomic exchange) and retry.
    // TODO: still O(ring) per exhaustion for alloc-heavy workflows -- some bookkeeping could keep this cheaper.
    auto base = start_base;
    do
    {
        CC_ASSERT(base != nullptr, "the slab ring must be a cycling single-linked-list. indicates a "
                                   "node_memory_resource bug.");

        auto const local = cc::node_slab_freemap_for_base(base);
        u64 freemap = *local;
#if CC_HAS_THREADS
        if (freemap == 0) // reclaim cross-thread frees into local
            freemap
                = std::atomic_ref<u64>(*cc::node_slab_remote_for_base(base, idx)).exchange(0, std::memory_order_relaxed);
#endif

        if (freemap != 0) [[likely]]
        {
            _slabs.slab_base[isize(idx)] = base; // keep allocating from this slab
            auto const slot_idx = cc::count_trailing_zeroes(freemap);
            *local = freemap & ~(u64(1) << slot_idx); // owner-only, non-atomic
            return cc::node_slot_ptr_for(base, idx, slot_idx);
        }

        base = cc::node_slab_next_for_base(base);
    } while (base != start_base);

    // whole ring exhausted (local + remote) => request a new slab
    return this->refill_slabs_and_allocate_node_bytes(idx);
}

cc::node_allocator& cc::default_node_allocator()
{
    return cc::default_node_memory_resource->get_allocator(cc::default_node_memory_resource->userdata);
}
