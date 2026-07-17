#include "node_allocation.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/thread/atomic.hh>

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

/// Splice a slab into the class's ring as the new head, retaining the existing slabs. Frontend-agnostic
/// (touches only next pointers); used by both fresh refill and orphan adoption.
void node_splice_slab_as_head(cc::node_allocator::slab_info& slabs, cc::node_class_index idx, cc::byte* new_slab)
{
    cc::byte*& head = slabs.slab_base[cc::isize(idx)];
    if (head == nullptr) // first slab for this class: a self-cycle
        *cc::node_slab_next_ptr_for_base(new_slab) = new_slab;
    else // insert after the old head so the whole ring stays reachable
    {
        *cc::node_slab_next_ptr_for_base(new_slab) = cc::node_slab_next_for_base(head);
        *cc::node_slab_next_ptr_for_base(head) = new_slab;
    }
    head = new_slab;
}

/// Drain a slab's remote frees into local (threaded) and report whether every usable slot is now free,
/// i.e. no live node points into it. Frontend-aware: the single-threaded frontend has no remote to drain.
bool node_slab_drain_and_is_fully_free(cc::byte* base, cc::node_class_index idx)
{
    cc::u64 local = *cc::node_slab_freemap_for_base(base);
#if CC_HAS_THREADS
    local |= cc::atomic_ref<cc::u64>(*cc::node_slab_remote_for_base(base, idx)).exchange(0, cc::memory_order_relaxed);
    *cc::node_slab_freemap_for_base(base) = local;
#endif
    return local == cc::node_seed_local_freemaps[cc::isize(idx)];
}

/// Return a slab to the backing resource. Valid only for a fully-free slab (no live nodes point into it).
void node_free_slab_to_backing(cc::byte* base, cc::node_class_index idx)
{
    cc::isize const slab_size = cc::node_slab_size_bytes_for_class(idx);
    cc::default_memory_resource->deallocate_bytes(base, slab_size, slab_size, cc::default_memory_resource->userdata);
}

// how many cold-path (ring-exhaustion) entries to skip between trim sweeps, per class. Trim is a rare
// O(ring) sweep, so amortize it; a long-lived thread with a stable working set never produces a fully-free
// slab and so never actually frees anything regardless of how often the sweep runs.
constexpr cc::u16 node_trim_period = 64;

/// Reclaim surplus fully-free slabs of a class back to the backing resource, keeping the ring non-empty and
/// retaining exactly one fully-free slab as a spare (so steady churn never re-mallocs). Cold path only.
void node_trim_ring(cc::node_allocator::slab_info& slabs, cc::node_class_index idx)
{
    cc::byte* const head = slabs.slab_base[cc::isize(idx)];
    if (head == nullptr)
        return;

    // gather the ring into a local buffer; rings are small (bounded working set). skip if pathologically big.
    constexpr int cap = 64;
    cc::byte* ring[cap];
    int n = 0;
    for (cc::byte* cur = head;;)
    {
        if (n == cap)
            return; // unexpectedly large ring: leave it alone this sweep
        ring[n++] = cur;
        cur = cc::node_slab_next_for_base(cur);
        if (cur == head)
            break;
    }

    // survivors = every non-fully-free slab + one fully-free spare; free the rest. never empties the ring.
    cc::byte* survivors[cap];
    int s = 0;
    bool kept_spare = false;
    for (int i = 0; i < n; ++i)
    {
        if (node_slab_drain_and_is_fully_free(ring[i], idx))
        {
            if (!kept_spare)
            {
                kept_spare = true;
                survivors[s++] = ring[i];
            }
            else
                node_free_slab_to_backing(ring[i], idx);
        }
        else
            survivors[s++] = ring[i];
    }

    // rebuild the cyclic ring from the survivors (s >= 1 always: a non-empty ring keeps >= 1 slab)
    for (int i = 0; i < s; ++i)
        *cc::node_slab_next_ptr_for_base(survivors[i]) = survivors[(i + 1) % s];
    slabs.slab_base[cc::isize(idx)] = survivors[0];
}

#if CC_HAS_THREADS
// ---- slab lifecycle across threads: orphan bins + reclamation helpers ---------------------------------
// When a thread exits, its retained slabs are reclaimed: fully-free slabs go back to the backing resource,
// the rest are handed to a per-class orphan bin, and a later thread adopts an orphan on refill instead of
// mallocing fresh. The dying thread's owner_id is never recycled, so every free into an orphaned slab routes
// to remote until it is adopted -- no plain-local writer can race the future adopter. See the lifecycle
// section of libs/base/clean-core/docs/systems/node-allocation.md.
//
// The bin is a constinit cc::atomic_flag spinlock, not a std::mutex: it is trivially destructible and
// never torn down, so the main thread's tls_allocator can reclaim safely even during static teardown (a
// std::mutex static might already be destroyed). Abandonment/adoption are rare and latency-tolerant, so a
// spinlock's short holds are fine. The lock's release/acquire is the sole ordering for the handoff of a
// slab's local/owner/next fields between the exiting thread and a future adopter.
struct slab_orphan_bin
{
    cc::atomic_flag lock_flag; // C++20: default-cleared, trivially destructible
    cc::byte* head = nullptr;  // singly-linked via each slab's next field; nullptr-terminated (not a ring)

    void lock()
    {
        while (lock_flag.test_and_set(cc::memory_order_acquire))
            ; // spin: held only for a handful of pointer ops (plus, on reclaim, a rare backing free)
    }
    void unlock() { lock_flag.clear(cc::memory_order_release); }
};

constinit slab_orphan_bin s_orphans[cc::isize(cc::node_class_index::small_count)] = {};

/// Push an owned slab onto its class's orphan bin. The bin lock must already be held.
void push_orphan_locked(cc::node_class_index idx, cc::byte* slab)
{
    auto& bin = s_orphans[cc::isize(idx)];
    *cc::node_slab_next_ptr_for_base(slab) = bin.head;
    bin.head = slab;
}

/// Pop one orphan of the given class, or nullptr if the bin is empty. Takes the bin lock internally.
cc::byte* pop_orphan(cc::node_class_index idx)
{
    auto& bin = s_orphans[cc::isize(idx)];
    bin.lock();
    cc::byte* const slab = bin.head;
    if (slab != nullptr)
        bin.head = cc::node_slab_next_for_base(slab);
    bin.unlock();
    return slab;
}

/// Reclaim a thread's retained slabs at allocator teardown (exit-only). Per class: drain each slab's remote
/// into local; fully-free slabs go back to backing, the rest are orphaned for a later thread to adopt.
void system_reclaim_slabs(cc::node_allocator::slab_info& slabs, void* userdata)
{
    CC_UNUSED(userdata);
    for (cc::isize ci = 0; ci < cc::isize(cc::node_class_index::small_count); ++ci)
    {
        auto const idx = cc::node_class_index(ci);
        cc::byte* const head = slabs.slab_base[ci];
        if (head == nullptr)
            continue;

        // one lock per class: a single release publishes every pushed slab to a future adopter
        auto& bin = s_orphans[ci];
        bin.lock();
        cc::byte* cur = head;
        do
        {
            cc::byte* const nxt = cc::node_slab_next_for_base(cur); // read next before push/free clobbers cur
            if (node_slab_drain_and_is_fully_free(cur, idx))
                node_free_slab_to_backing(cur, idx);
            else
            {
                // only the exiting owner ever reaches here; the never-recycled token proves no live sharer
                CC_ASSERT(*cc::node_slab_owner_for_base(cur) == cc::node_owner_token(),
                          "reclaim must only orphan slabs still owned by the calling thread");
                push_orphan_locked(idx, cur);
            }
            cur = nxt;
        } while (cur != head);
        bin.unlock();

        slabs.slab_base[ci] = nullptr;
    }
}
#endif // CC_HAS_THREADS

/// The system resource's per-thread allocator, plus self-deregistration.
/// This is the one allocator that stays installed as the thread default until thread exit, so a
/// plain ~node_allocator assert would fire on it. Clearing the slot here is sound precisely where
/// the general case is not: this wrapper is itself thread_local, so its dtor provably runs on the
/// one thread whose slot it clears. detail::default_node_alloc is trivially destructible, so its
/// storage outlives this dtor on both the Itanium and MSVC TLS teardown paths.
struct tls_default_allocator
{
    cc::node_allocator alloc{&system_node_memory_resource};

    ~tls_default_allocator()
    {
        if (cc::detail::default_node_alloc == &alloc)
            cc::detail::default_node_alloc = nullptr;
    }
};

/// Returns a thread-local node_allocator for the system node memory resource.
/// The allocator is lazy-initialized with all slabs nullptr.
cc::node_allocator& system_get_allocator(void* userdata)
{
    CC_UNUSED(userdata);
    thread_local tls_default_allocator tls_allocator;
    return tls_allocator.alloc;
}

// 24-byte large-node header, sitting immediately BEFORE the returned payload: [size][alignment][resource*].
// The resource pointer is at payload-8 so node_allocation_free_large can recover it (see its contract).
constexpr cc::isize node_large_header_size = 24;

/// Allocates a large node (> small_max) from the system memory resource.
/// Layout: allocate a block aligned to `alignment`, place the payload at the first aligned offset that
/// leaves room for the 24-byte header, and write the header in the 24 bytes right before the payload. So
/// the payload honors any power-of-two alignment (not just 8) while keeping resource* at payload-8.
cc::byte* system_allocate_node_bytes_large(cc::node_class_index idx, cc::isize size_bytes, cc::isize alignment, void* userdata)
{
    CC_UNUSED(idx); // not needed for system allocator
    CC_UNUSED(userdata);

    // bump alignment to at least 8 bytes (the header fields are 8-byte writes); must be a power of two
    alignment = cc::max(alignment, cc::isize(8));

    // payload offset: the first `alignment`-aligned point with >= header_size bytes ahead of it for the
    // header. Since the block itself is `alignment`-aligned, this offset is a multiple of alignment, so the
    // payload is aligned too. For alignment 8 this is 24 (the old layout); for 16 it is 32; etc.
    cc::isize const payload_offset = cc::align_up(node_large_header_size, alignment);
    cc::isize const total_size = payload_offset + size_bytes;

    // allocate from system memory resource, aligned so the payload lands on an `alignment` boundary
    cc::byte* alloc_ptr = nullptr;
    cc::isize const actual_size = cc::default_memory_resource->allocate_bytes(
        &alloc_ptr, total_size, total_size, alignment, cc::default_memory_resource->userdata);
    CC_ASSERT(actual_size >= total_size, "system allocator must allocate at least the requested size");
    CC_ASSERT(alloc_ptr != nullptr, "system allocator must return non-null for non-zero size");

    cc::byte* const payload = alloc_ptr + payload_offset;
    CC_ASSERT(cc::is_aligned(payload, alignment), "large-node payload must honor the requested alignment");

    // write the header in the 24 bytes right before the payload: [size][alignment][resource*]
    *reinterpret_cast<cc::isize*>(payload - 24) = size_bytes;                                  // NOLINT
    *reinterpret_cast<cc::isize*>(payload - 16) = alignment;                                   // NOLINT
    *reinterpret_cast<cc::node_memory_resource**>(payload - 8) = &system_node_memory_resource; // NOLINT

    return payload;
}

/// Deallocates a large node (> small_max) by reading the header and calling into system memory resource.
void system_deallocate_node_bytes_large(cc::byte* ptr, cc::node_class_index idx, void* userdata)
{
    CC_UNUSED(idx); // not needed for system allocator
    CC_UNUSED(userdata);

    // read the header from the 24 bytes before the payload: [size][alignment][resource*]
    cc::isize const size_bytes = *reinterpret_cast<cc::isize*>(ptr - 24);         // NOLINT
    cc::isize const alignment = *reinterpret_cast<cc::isize*>(ptr - 16);          // NOLINT
    auto const resource = *reinterpret_cast<cc::node_memory_resource**>(ptr - 8); // NOLINT

    CC_ASSERT(resource == &system_node_memory_resource, "resource mismatch in large node deallocation");

    // recover the original allocation: the payload sits `payload_offset` into it (same formula as alloc)
    cc::isize const payload_offset = cc::align_up(node_large_header_size, alignment);
    cc::byte* const alloc_ptr = ptr - payload_offset;
    cc::isize const total_size = payload_offset + size_bytes;
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

#if CC_HAS_THREADS
    // adoption: reuse an orphaned slab of this class (abandoned by an exited thread) before mallocing fresh.
    if (cc::byte* const orphan = pop_orphan(idx); orphan != nullptr)
    {
        // take ownership. atomic store because remote-freeing threads read owner_id concurrently on their
        // free path -- they route to remote whichever value they see, but the write must not tear.
        cc::atomic_ref<cc::u32>(*cc::node_slab_owner_for_base(orphan)).store(cc::node_owner_token(), cc::memory_order_relaxed);
        // drain remote frees accumulated while orphaned into local
        cc::u64 local = *cc::node_slab_freemap_for_base(orphan);
        local
            |= cc::atomic_ref<cc::u64>(*cc::node_slab_remote_for_base(orphan, idx)).exchange(0, cc::memory_order_relaxed);
        *cc::node_slab_freemap_for_base(orphan) = local;
        // track it in the ring regardless of capacity (future remote frees drain on the cold walk)
        node_splice_slab_as_head(slabs, idx, orphan);
        if (local != 0) // adopted a slab with capacity: allocate from it and we are done
        {
            auto const slot_idx = cc::count_trailing_zeroes(local);
            *cc::node_slab_freemap_for_base(orphan) = local & ~(cc::u64(1) << slot_idx);
            return cc::node_slot_ptr_for(orphan, idx, slot_idx);
        }
        // orphan was full (all slots handed to others): fall through to malloc a fresh slab for this alloc
    }
#endif

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
    node_splice_slab_as_head(slabs, idx, new_slab);

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
#if CC_HAS_THREADS
    .reclaim_slabs = system_reclaim_slabs,
#endif
    .userdata = nullptr,
};

} // namespace

constinit cc::node_memory_resource* const cc::default_node_memory_resource = &system_node_memory_resource;

#if CC_HAS_THREADS
cc::u32 cc::detail::node_next_owner_id()
{
    // process-unique, never recycled: an id is never reused, so a free is never miscategorized.
    // ids are not reclaimed on thread exit (that leaks the thread's slabs -- a known, deferred follow-up).
    static cc::atomic<cc::u32> s_next_owner_id{1}; // 0 is reserved for "unassigned"
    cc::u32 const id = s_next_owner_id.fetch_add(1, cc::memory_order_relaxed);
    CC_ASSERT(id != 0, "node owner-id space exhausted (>4B threads ever); cross-thread-free after "
                       "thread-exit is unsupported");
    return id;
}

cc::isize cc::detail::node_orphan_slab_count()
{
    cc::isize count = 0;
    for (cc::isize ci = 0; ci < cc::isize(cc::node_class_index::small_count); ++ci)
    {
        auto& bin = s_orphans[ci];
        bin.lock();
        for (cc::byte* s = bin.head; s != nullptr; s = cc::node_slab_next_for_base(s))
            ++count;
        bin.unlock();
    }
    return count;
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
    // rare, gated trim: return surplus fully-free slabs (from a past watermark) to the backing resource.
    // cold path only, so the hot alloc/free codegen is unchanged; a stable working set never trims (no slab
    // is ever fully free), so long-lived threads pay nothing but an occasional cheap counter bump + walk.
    if (_slabs.slab_base[isize(idx)] != nullptr && ++_slabs.trim_gate[isize(idx)] >= node_trim_period)
    {
        _slabs.trim_gate[isize(idx)] = 0;
        node_trim_ring(_slabs, idx);
    }

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
                = cc::atomic_ref<u64>(*cc::node_slab_remote_for_base(base, idx)).exchange(0, cc::memory_order_relaxed);
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

cc::node_allocator* cc::detail::node_alloc_hydrate_default()
{
    auto* const a = &cc::default_node_memory_resource->get_allocator(cc::default_node_memory_resource->userdata);
    cc::detail::default_node_alloc = a;
    return a;
}

void cc::set_default_node_allocator(cc::node_allocator* alloc)
{
    cc::detail::default_node_alloc = alloc;
}

cc::node_allocator* cc::get_default_node_allocator()
{
    return cc::detail::default_node_alloc;
}

cc::node_allocator::~node_allocator()
{
    // A destroyed allocator that is still installed leaves the slot dangling, and the next alloc on
    // this thread hands out slots from freed slabs. Best-effort: only this thread's slot is visible,
    // so a cross-thread install/destroy still slips through. The system's own thread-default is
    // exempt because tls_default_allocator clears the slot before we get here.
    CC_ASSERT(cc::detail::default_node_alloc != this,
              "node allocator destroyed while still installed as this thread's default -- deregister it first "
              "(set_default_node_allocator / scoped_default_node_allocator)");

#if CC_HAS_THREADS
    // hand our retained slabs back to the resource so later threads adopt them instead of leaking.
    // no-op for resources without the hook (they keep today's leak-on-exit behavior).
    if (_resource != nullptr && _resource->reclaim_slabs != nullptr)
        _resource->reclaim_slabs(_slabs, _resource->userdata);
#endif
}
