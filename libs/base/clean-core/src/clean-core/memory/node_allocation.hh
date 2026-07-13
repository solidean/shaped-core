#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

#include <atomic>

/// Newtype for size class index (0, 1, 2, ...) to prevent API confusion.
/// The index i maps to actual size 2^i bytes.
/// Kept as u8 to be as tight as possible
enum class cc::node_class_index : cc::u8
{
    // max small class is 2^8 = 256 bytes
    // after that we do normal allocations and store a header
    small_max = 8,

    // number of small size classes
    small_count = small_max + 1,
};

/// Newtype for actual size class in bytes (1, 2, 4, 8, 16, ...) to prevent API confusion.
/// This is the actual allocation size & alignment, computed as 2^index.
enum class cc::node_class_size : cc::u64
{
};

namespace cc
{
/// Compute the class index from size and alignment requirements.
/// Class index i maps to actual size 2^i bytes (power of two).
/// Input is max(size, alignment) rounded up to next power of 2 via bit_width.
/// This helper is called by the templated node_class_index_for<T>() to minimize template logic.
/// Examples: size=1,align=1 → max=1 → bit_width=1 → index=0 (class size 1)
///           size=5,align=4 → max=5 → bit_width(4)=3 → index=3 (class size 8)
///           size=16,align=8 → max=16 → bit_width(15)=4 → index=4 (class size 16)
[[nodiscard]] constexpr node_class_index node_class_index_from_size_and_align(isize size, isize alignment)
{
    auto const required = cc::max(size, alignment);
    return node_class_index(cc::bit_width(u64(required - 1)));
}

/// Compute the class index for T.
/// Class index i means actual allocation size is 2^i bytes.
/// All nodes with the same class index share slab infrastructure.
/// Uses bit operations for ultra-cheap computation and masking logic.
/// Examples: sizeof=1,alignof=1 → index 0 (size 1)
///           sizeof=12,alignof=4 → index 4 (size 16)
///           sizeof=24,alignof=8 → index 5 (size 32)
template <class T>
[[nodiscard]] constexpr node_class_index node_class_index_for()
{
    return node_class_index_from_size_and_align(sizeof(T), alignof(T));
}

/// Compute slab size in bytes for a given class index.
/// Slab size = (2^index) * 64 bytes, yielding exactly 64 slots per slab.
/// Examples: index 0 (size 1) → 64 bytes, index 2 (size 4) → 256 bytes, index 4 (size 16) → 1024 bytes.
[[nodiscard]] constexpr isize node_slab_size_bytes_for_class(node_class_index idx)
{
    return (isize(1) << isize(idx)) * 64;
}

/// Compute the alignment mask for a slab of the given class index.
/// Used to extract slab base from any pointer within the slab via ptr & ~mask.
[[nodiscard]] constexpr isize node_slab_mask_for_class(node_class_index idx)
{
    return node_slab_size_bytes_for_class(idx) - 1;
}

/// Slab metadata byte layout (kept in one place so refill, the fast path, and the seed mask agree):
///   CC_HAS_THREADS==1: local_freemap @0 (u64), owner_id @8 (u32), next @16 (byte*); remote_freemap on
///                      the 2nd cache line @64 (u64) when the slab spans two lines (class idx >= 1), else
///                      @24 (the 1 B class has a single-line 64 B slab, so its remote shares line 0).
///   CC_HAS_THREADS==0: local_freemap @0 (u64), next @8 (byte*). No owner/remote.
/// Data still starts at slab offset 0; slots overlapping metadata are permanently blocked by the seed mask.

/// Offset of the next-slab-ring pointer, which moves to make room for owner_id in the threaded layout.
inline constexpr isize node_slab_next_offset = CC_HAS_THREADS ? 16 : 8;

/// Compute the initial local free bitmap for a freshly refilled slab of the given class.
/// Bit i is 1 (free) unless slot i overlaps slab metadata, in which case it is permanently 0.
/// consteval so each class's mask is a compile-time constant; the runtime-idx refill reads the table below.
[[nodiscard]] consteval u64 node_compute_seed_local_freemap(int idx)
{
    isize const size = isize(1) << idx;
    isize const slab = size * 64;
    u64 mask = ~u64(0); // all slots free

    // clear every slot touched by the metadata byte range [lo, hi)
    auto block = [&](isize lo, isize hi)
    {
        isize const first = lo / size;
        isize const last = (hi - 1) / size;
        for (isize s = first; s <= last; ++s)
            mask &= ~(u64(1) << s);
    };

#if CC_HAS_THREADS
    block(0, 8);   // local_freemap
    block(8, 12);  // owner_id (u32)
    block(16, 24); // next
    if (slab >= 128)
        block(64, 72); // remote_freemap on the 2nd cache line
    else
        block(24, 32); // 1 B class: remote shares line 0
#else
    block(0, 8);  // local_freemap
    block(8, 16); // next
#endif
    return mask;
}

/// Per-class initial local free bitmaps, one per small size class. Indexed by class index in refill.
inline constexpr u64 node_seed_local_freemaps[isize(node_class_index::small_count)] = {
    node_compute_seed_local_freemap(0), node_compute_seed_local_freemap(1), node_compute_seed_local_freemap(2),
    node_compute_seed_local_freemap(3), node_compute_seed_local_freemap(4), node_compute_seed_local_freemap(5),
    node_compute_seed_local_freemap(6), node_compute_seed_local_freemap(7), node_compute_seed_local_freemap(8),
};
static_assert(cc::popcount(node_seed_local_freemaps[0]) >= 1, "every class must have at least one usable slot");
static_assert(cc::popcount(node_seed_local_freemaps[isize(node_class_index::small_max)]) >= 1,
              "every class must have at least one usable slot");

/// Recover the slab base address from any pointer inside the slab.
/// Exploits the fact that slabs are aligned to their own size.
/// Computation is ptr & ~(slab_size - 1), a single bitwise AND.
[[nodiscard]] CC_FORCE_INLINE cc::byte* node_slab_base_for_ptr(cc::byte* ptr, node_class_index idx)
{
    auto const mask = node_slab_mask_for_class(idx);
    return reinterpret_cast<cc::byte*>(reinterpret_cast<u64>(ptr) & ~u64(mask)); // NOLINT
}

/// Retrieve the free bitmap for a slab.
/// The u64 free bitmap is stored at the slab base; one bit per slot.
/// Bits corresponding to slots overlapping the bitmap itself remain permanently zero.
/// This function is safe to call with nullptr and will return nullptr in that case.
[[nodiscard]] CC_FORCE_INLINE u64* node_slab_freemap_for_base(cc::byte* base)
{
    return reinterpret_cast<u64*>(base); // NOLINT
}

/// Retrieve a pointer to the next-slab-ring slot for read or write.
/// The offset depends on the frontend (see node_slab_next_offset); all next-pointer access goes through here.
[[nodiscard]] CC_FORCE_INLINE cc::byte** node_slab_next_ptr_for_base(cc::byte* base)
{
    return reinterpret_cast<cc::byte**>(base + node_slab_next_offset); // NOLINT
}

/// Retrieve the next slab in the ring.
[[nodiscard]] CC_FORCE_INLINE cc::byte* node_slab_next_for_base(cc::byte* base)
{
    return *node_slab_next_ptr_for_base(base);
}

#if CC_HAS_THREADS
/// Retrieve the owner-id slot of a slab (threaded frontend only).
/// Holds the token of the thread that hydrated the slab; a free from any other thread routes to remote.
[[nodiscard]] CC_FORCE_INLINE u32* node_slab_owner_for_base(cc::byte* base)
{
    return reinterpret_cast<u32*>(base + 8); // NOLINT
}

/// Retrieve the remote free bitmap of a slab (threaded frontend only).
/// Lives on the 2nd cache line (@64) for classes whose slab spans two lines; the single-line 1 B class
/// keeps it on line 0 (@24). Remote frees atomic_or here; the owner drains it into local on underflow.
[[nodiscard]] CC_FORCE_INLINE u64* node_slab_remote_for_base(cc::byte* base, node_class_index idx)
{
    auto const off = (node_slab_size_bytes_for_class(idx) >= 128) ? isize(64) : isize(24);
    return reinterpret_cast<u64*>(base + off); // NOLINT
}

namespace detail
{
/// Allocates the next process-unique nonzero owner id. Out-of-line so the token fast path stays a TLS load.
[[nodiscard]] CC_COLD_FUNC u32 node_next_owner_id();

/// Test/debug hook: total slabs currently held in the system resource's per-class orphan bins (slabs
/// abandoned by exited threads, awaiting adoption). Locks each bin; not for hot use.
[[nodiscard]] isize node_orphan_slab_count();

/// Per-thread owner token storage (one instance per thread across TUs). 0 == unassigned.
/// node_owner_token() assigns it lazily on first alloc/hydrate; the hot free path reads it raw.
inline thread_local u32 owner_token = 0;
} // namespace detail

/// Process-unique, never-recycled token for the calling thread (threaded frontend only).
/// Lazily assigned once per thread; used to stamp slabs at hydrate/adoption and on the allocating path.
[[nodiscard]] CC_FORCE_INLINE u32 node_owner_token()
{
    if (detail::owner_token == 0) [[unlikely]]
        detail::owner_token = cc::detail::node_next_owner_id();
    return detail::owner_token;
}

/// The calling thread's owner token WITHOUT lazy assignment (0 if never assigned). For the hot free path:
/// a thread with token 0 has never hydrated a slab, so it owns none, so 0 != any slab's nonzero owner and
/// the free correctly routes to remote. Skipping the assignment drops a branch (the t==0 check) from every free.
[[nodiscard]] CC_FORCE_INLINE u32 node_owner_token_or_zero()
{
    return detail::owner_token;
}
#endif

/// Compute the slot index within a slab for a given pointer.
/// Index = (ptr - slab_base) / class_size where class_size = 2^class_index.
/// Optimized to (ptr - slab_base) >> class_index for power-of-two division.
[[nodiscard]] CC_FORCE_INLINE u64 node_slot_index_for_ptr(cc::byte* ptr, cc::byte* base, node_class_index idx)
{
    return u64(ptr - base) >> u64(idx);
}

/// Compute the slot pointer for a given base address, class index, and slot index.
/// Inverse of node_slot_index_for_ptr: ptr = base + slot_index * class_size.
/// Optimized to base + (slot_index << class_index) for power-of-two multiplication.
[[nodiscard]] CC_FORCE_INLINE cc::byte* node_slot_ptr_for(cc::byte* base, node_class_index idx, u64 slot_index)
{
    return base + (slot_index << u64(idx));
}

/// Cold path for freeing large nodes (> small_max).
/// Called by node_allocation_free when idx > small_max.
/// Delegates to the resource's deallocate_node_bytes_large function pointer.
CC_COLD_FUNC void node_allocation_free_large(cc::byte* ptr, node_class_index idx);

/// Free a node by returning its slot to the slab's free bitmap.
/// Requires only the pointer and class index; no allocator state or resource reference needed.
/// The owning thread frees non-atomically into local; a genuinely remote thread atomic_ors into the slab's
/// remote bitmap, which the owner drains on underflow. Single-threaded builds always take the plain path.
///
/// DESIGN CHOICE: This is a free function, not bound to node_memory_resource.
/// This ensures that freeing cannot directly depend on the resource by default.
/// Deallocation is stateless and decoupled from allocation, enabling cross-thread deallocation
/// without requiring the freeing thread to have any reference to or knowledge of the allocating resource.
CC_FORCE_INLINE void node_allocation_free(cc::byte* ptr, node_class_index idx)
{
    // branch for large nodes
    if (idx > node_class_index::small_max) [[unlikely]]
    {
        cc::node_allocation_free_large(ptr, idx);
        return;
    }

    auto const base = cc::node_slab_base_for_ptr(ptr, idx);
    auto const slot_bit = u64(1) << cc::node_slot_index_for_ptr(ptr, base, idx);

#if CC_HAS_THREADS
    // owner path: non-atomic free into local; predicted-taken because most frees are on the owning thread.
    // raw token read (no lazy-init): a thread that never allocated has token 0, owns nothing, so it routes
    // to remote -- correct, and it keeps the t==0 branch out of the hot free.
    if (*cc::node_slab_owner_for_base(base) == cc::node_owner_token_or_zero()) [[likely]]
    {
        auto const freemap = cc::node_slab_freemap_for_base(base);
        CC_ASSERT((*freemap & slot_bit) == 0, "node is already freed. double-delete or corruption?");
        *freemap |= slot_bit;
    }
    else // remote thread: the only path that pays an atomic (double-free assert omitted; the read would race)
    {
        std::atomic_ref<u64>(*cc::node_slab_remote_for_base(base, idx)).fetch_or(slot_bit, std::memory_order_relaxed);
    }
#else
    auto const freemap = cc::node_slab_freemap_for_base(base);
    CC_ASSERT((*freemap & slot_bit) == 0, "node is already freed. double-delete or corruption?");
    *freemap |= slot_bit;
#endif
}

/// Default node memory resource used when none specified.
/// This is a system-backed allocator stored in the data segment, making the pointer valid even during
/// static initialization in other translation units (safe for use in global/static constructors).
/// This uses TLS-pooled slabs per size class and a cold-path rebalance strategy
/// New backing allocations are pulled in from cc::default_memory_resource
extern cc::node_memory_resource* const default_node_memory_resource;
} // namespace cc

// this is a concrete non-customizable allocator interface for all node classes
// if a node memory resource segregates by threads, it must be by giving out different allocators (e.g. via TLS)
// this is designed in a way that the fast path allocation is _extremely_ fast
// each node_allocator is NOT thread_safe, even though a node_memory_resource might be
// this struct functions as a local cache to keep the hot path happy
struct cc::node_allocator
{
public:
    struct slab_info
    {
        // base pointers to a slab for each class
        // is lazy-initialized (can be nullptr) via a slow path
        // concrete node memory resources can decide to only support certain classes (but the default one supports all)
        // the slabs here are not guaranteed to have free slots
        // because we want to keep the happy path fast (and there could be frees until the next alloc)
        cc::byte* slab_base[isize(node_class_index::small_count)] = {};

        // per-class cold-path counter: gates the rare O(ring) trim sweep (see node_trim_ring). Allocator-side
        // bookkeeping only -- the hot path never reads it, so it does not touch slab metadata or codegen.
        cc::u16 trim_gate[isize(node_class_index::small_count)] = {};
    };

    // ctors
    node_allocator() = default;
    explicit node_allocator(node_memory_resource* resource) : _resource(resource) {}

    // at teardown, hand retained slabs back to the resource (via its reclaim_slabs hook) so a thread's
    // slabs are adopted by later threads instead of leaking. no-op if the resource has no hook.
    // move/copy are deleted: an allocator owns its slab ring and is only ever used by reference or in place.
    ~node_allocator();
    node_allocator(node_allocator const&) = delete;
    node_allocator& operator=(node_allocator const&) = delete;
    node_allocator(node_allocator&&) = delete;
    node_allocator& operator=(node_allocator&&) = delete;

    // accessors
    [[nodiscard]] slab_info& slabs() { return _slabs; }
    [[nodiscard]] slab_info const& slabs() const { return _slabs; }

public:
    // allocates a new node from the given size class
    // must be freed via node_allocation_free and the same class index!
    // extremely efficient fast path
    // occasionally calls into a slow path to regenerate
    // NOTE: this must be inlined to make "idx" a comptime constant in most cases!
    // size_bytes and alignment are only used for large nodes (idx > small_max) and ignored for small classes
    [[nodiscard]] CC_FORCE_INLINE cc::byte* allocate_node_bytes(node_class_index idx, isize size_bytes, isize alignment)
    {
        // slow path for large nodes
        if (idx > node_class_index::small_max) [[unlikely]]
            return this->allocate_node_bytes_large(idx, size_bytes, alignment);

        auto const base = _slabs.slab_base[isize(idx)];

        // happy path for valid slabs with free slots
        if (base != nullptr) [[likely]]
        {
            // only the owning thread allocates from local, so this is a plain (non-atomic) read/clear.
            // remote frees land in a separate bitmap and are drained into local by the cold path.
            auto const local = cc::node_slab_freemap_for_base(base);
            auto const freemap = *local;

            if (freemap != 0) [[likely]]
            {
                auto const slot_idx = cc::count_trailing_zeroes(freemap);
                auto const slot_bit = u64(1) << slot_idx;
                *local = freemap & ~slot_bit;
                return cc::node_slot_ptr_for(base, idx, u64(slot_idx));
            }
        }

        // all else goes through fallback paths (local empty: drain remote / walk ring / refill)
        return this->allocate_node_bytes_non_fast(idx);
    }

private:
    // fallback when the current slab is either not allocated or doesn't have free slots
    [[nodiscard]] cc::byte* allocate_node_bytes_non_fast(node_class_index idx);

    // slow path for non-small nodes
    [[nodiscard]] CC_COLD_FUNC cc::byte* allocate_node_bytes_large(node_class_index idx, isize size_bytes, isize alignment);

    // called when the current slab ring is full
    // ensures the slab ring has free capacity and allocates a node for the given (small) class
    // also works when the class is not initialized yet
    [[nodiscard]] CC_COLD_FUNC cc::byte* refill_slabs_and_allocate_node_bytes(node_class_index idx);

private:
    // slab configuration
    slab_info _slabs;

    // the backup resource for this allocation
    // NOTE: must be non-nullptr for a valid allocator!
    cc::node_memory_resource* _resource = nullptr;
};

/// Small-node allocation system optimized for cheap thread-local allocation and wait-free deallocation.
/// Nodes are grouped by power-of-two size classes: class index i corresponds to size 2^i bytes.
/// Size class index = bit_width(max(sizeof(T), alignof(T)) - 1), ensuring both size and alignment are satisfied.
///
/// Each slab is a 64 * class_size block; slabs are aligned to their own size. Data starts at slab offset 0;
/// slots overlapping the metadata are permanently blocked by a per-class consteval seed mask (see
/// node_compute_seed_local_freemap). The metadata layout is frontend-dependent (CC_HAS_THREADS):
///   - threaded: local_freemap @0, owner_id @8 (u32), next @16; remote_freemap @64 (2nd cache line) for
///     classes whose slab spans two lines (idx >= 1), else @24. Usable slots: 1B → 36, 2B → 50, 16B+ → ~62.
///   - single-threaded: local_freemap @0, next @8; usable slots: 1B → 48, 8B → 62, 16B+ → 63 (as before).
///
/// The fast path is two compile-time frontends behind an unchanged API (selected by CC_HAS_THREADS):
///   - threaded (step2_tls_diff): the owner allocs/frees non-atomically into local; a genuinely remote
///     thread atomic_ors into remote; the owner drains remote into local on underflow (cold path).
///   - single-threaded: plain non-atomic and/or on one bitmap, no owner/remote.
/// Deallocation requires only the pointer and class index; no allocator state or resource reference needed.
/// The allocating thread discovers remotely freed slots during cold allocation paths (drain + slab reuse).
///
/// Slab base recovery from any interior pointer: ptr & ~(slab_size - 1), exploiting alignment.
///
/// Target use case: node-based containers (list, map, set) and small heap objects (unique_ptr payloads, unique_function).
/// Out of scope: large allocations, contiguous buffers, bulk operations; use cc::allocation<T> or cc::memory_resource.
///
/// Future: could support shared_ptr-like semantics as well by co-locating refcounts
struct cc::node_memory_resource
{
    // returns a node allocator that is usable on this thread
    // must not be used cross-thread
    // for best performance, try to reuse one allocator as much as possible
    // it is fine for a memory resource to return the same reference again on the same thread
    // this mechanism allows TLS-segregated resources but also purely local resources at the same time
    cc::function_ptr<node_allocator&(void*)> get_allocator = nullptr;

    // allocates a large node (> small_max) from the resource
    // called by node_allocator::allocate_node_bytes_large
    // receives class index, actual size in bytes, and alignment requirement
    // REQUIREMENT: returned ptr must be at least 8-byte aligned
    // REQUIREMENT: ptr - 8 must contain a pointer to this resource (node_memory_resource*)
    //              this allows node_allocation_free_large to retrieve the resource for deallocation
    cc::function_ptr<cc::byte*(node_class_index, isize, isize, void*)> allocate_node_bytes_large = nullptr;

    // refills slabs for the given size class and allocates a node from the new slab
    // called by node_allocator::refill_slabs_and_allocate_node_bytes
    // takes a reference to the allocator's slab_info and the class index
    cc::function_ptr<cc::byte*(node_allocator::slab_info&, node_class_index, void*)> refill_slabs_and_allocate_node_bytes
        = nullptr;

    // deallocates a large node (> small_max) to the resource
    // called by node_allocation_free_large
    cc::function_ptr<void(cc::byte*, node_class_index, void*)> deallocate_node_bytes_large = nullptr;

    // optional: hand this allocator's retained slabs back to the resource at allocator teardown.
    // called from ~node_allocator (threaded frontend only). null => slabs leak on destruction, which is
    // fine for stateless/private resources; the system resource implements per-class orphan reclamation so
    // a thread's slabs are reused by later threads instead of leaking. must null out each slab_base it takes.
    cc::function_ptr<void(node_allocator::slab_info&, void*)> reclaim_slabs = nullptr;

    /// User-defined data for custom allocators. Can be nullptr for stateless allocators.
    void* userdata = nullptr;
};

namespace cc
{
/// Returns the default node allocator for the current thread.
/// This is a convenience wrapper around default_node_memory_resource->get_allocator.
/// The returned allocator is thread-local and must not be used across threads.
/// For most use cases, this is the recommended way to obtain a node allocator (or take an explicit node_allocator&).
node_allocator& default_node_allocator();
} // namespace cc

/// Move-only owning handle for a single live T stored in node memory.
/// Stores only a T*; all information required to free the slot is derived from the pointer and sizeof/alignof(T).
/// The destructor calls ~T() and returns the slot to its slab via wait-free bitmap update.
/// No allocator state, resource reference, or size parameter is stored or required for destruction.
/// Intended for node-based container elements, unique_ptr payloads, and unique_function storage.
/// This handle does not support copying; ownership transfer is move-only.
template <class T>
struct cc::node_allocation
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    // properties
public:
    [[nodiscard]] bool is_valid() const { return ptr != nullptr; }
    explicit operator bool() const { return ptr != nullptr; }

    // smart pointer interface
public:
    [[nodiscard]] T& operator*() const
    {
        CC_ASSERT(ptr != nullptr, "dereferencing null node_allocation");
        return *ptr;
    }
    [[nodiscard]] T* operator->() const
    {
        CC_ASSERT(ptr != nullptr, "dereferencing null node_allocation");
        return ptr;
    }

    // factory
public:
    // NOTE: resource can be nullptr
    template <class... Args>
    [[nodiscard]] static node_allocation create_from(node_allocator& alloc, Args&&... args)
    {
        static_assert(
            requires { T(cc::forward<Args>(args)...); }, "T is not constructible from the provided argument "
                                                         "types");

        auto const ptr = alloc.allocate_node_bytes(cc::node_class_index_for<T>(), sizeof(T), alignof(T));

        node_allocation n;
        n.ptr = new (cc::placement_new, ptr) T(cc::forward<Args>(args)...);
        return n;
    }

    // ctors/dtor
public:
    node_allocation() = default;

    node_allocation(node_allocation&& rhs) noexcept : ptr(cc::exchange(rhs.ptr, nullptr)) {}
    node_allocation& operator=(node_allocation&& rhs) noexcept
    {
        // take ownership from rhs while it's definitely still alive
        T* new_ptr = cc::exchange(rhs.ptr, nullptr);

        // now release current ownership
        reset();

        // and adopt the stolen pointer
        ptr = new_ptr;
        return *this;
    }
    node_allocation(node_allocation const&) = delete;
    node_allocation& operator=(node_allocation const&) = delete;

    /// Destroy the managed object and return its slot to the slab.
    /// Calls ~T(), then performs a single atomic_or into the slab's free bitmap.
    /// Wait-free and requires no allocator state; the slab base and slot index are derived from ptr.
    ~node_allocation()
    {
        if (ptr != nullptr)
        {
            ptr->~T();
            cc::node_allocation_free((cc::byte*)ptr, cc::node_class_index_for<T>());
        }
    }

    /// Destroy the managed object (if any) and reset to empty state.
    /// Safe to call on an already-empty handle.
    void reset()
    {
        if (ptr != nullptr)
        {
            ptr->~T();
            cc::node_allocation_free((cc::byte*)ptr, cc::node_class_index_for<T>());
            ptr = nullptr;
        }
    }

    // members
public:
    /// Pointer to the managed T; nullptr indicates an empty handle.
    /// All deallocation metadata (slab base, slot index) is computed from this pointer.
    T* ptr = nullptr;
};

/// A node_allocation with its element type erased: a single move-only handle that owns any node-allocated
/// object, whatever its type or size class. It keeps node_allocation<T>'s allocation fast path and stateless
/// wait-free free, and is built by moving a node_allocation<T> in. The tradeoff is handle size — three
/// pointers (payload, deleter, class index) instead of one — so prefer node_allocation<T> when the type is
/// statically known. Meant for type-erased wrappers with no natural base class, e.g. unique_function storage.
///
/// How it works: erasing a node_allocation<T> captures its pointer, its class index, and a deleter (null for
/// trivially destructible T). Destruction runs the deleter (if any) and returns the slot using the stored
/// class index, so no type information or allocator state is needed at the free site.
struct cc::any_node_allocation
{
    // properties
public:
    [[nodiscard]] bool is_valid() const { return ptr != nullptr; }
    explicit operator bool() const { return ptr != nullptr; }

    // ctors/dtor
public:
    any_node_allocation() = default;

    template <class T>
    any_node_allocation(node_allocation<T> alloc)
      : ptr(cc::exchange(alloc.ptr, nullptr)), class_index(cc::node_class_index_for<T>())
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
            deleter = [](void* ptr) { (*reinterpret_cast<T*>(ptr)).~T(); }; // NOLINT
    }

    any_node_allocation(any_node_allocation&& rhs) noexcept
      : ptr(cc::exchange(rhs.ptr, nullptr)), deleter(cc::exchange(rhs.deleter, nullptr)), class_index(rhs.class_index)
    {
    }
    any_node_allocation& operator=(any_node_allocation&& rhs) noexcept
    {
        // take ownership from rhs while it's definitely still alive
        void* new_ptr = cc::exchange(rhs.ptr, nullptr);
        cc::function_ptr<void(void*)> new_deleter = cc::exchange(rhs.deleter, nullptr);
        cc::node_class_index const new_class_index = rhs.class_index;

        // now release current ownership
        reset();

        // and adopt the stolen pointer
        ptr = new_ptr;
        deleter = new_deleter;
        class_index = new_class_index;
        return *this;
    }
    any_node_allocation(any_node_allocation const&) = delete;
    any_node_allocation& operator=(any_node_allocation const&) = delete;

    /// Destroy the managed object and return its slot to the slab.
    /// Calls ~T(), then performs a single atomic_or into the slab's free bitmap.
    /// Wait-free and requires no allocator state; the slab base and slot index are derived from ptr.
    ~any_node_allocation()
    {
        if (ptr != nullptr)
        {
            if (deleter)
                deleter(ptr);

            cc::node_allocation_free(reinterpret_cast<cc::byte*>(ptr), class_index); // NOLINT
        }
    }

    /// Destroy the managed object (if any) and reset to empty state.
    /// Safe to call on an already-empty handle.
    void reset()
    {
        if (ptr != nullptr)
        {
            if (deleter)
                deleter(ptr);

            cc::node_allocation_free(reinterpret_cast<cc::byte*>(ptr), class_index); // NOLINT
            ptr = nullptr;
        }
    }

    // members
public:
    /// Pointer to the managed T; nullptr indicates an empty handle.
    /// All deallocation metadata (slab base, slot index) is computed from this pointer.
    void* ptr = nullptr;

    /// If non-null, calls the dtor of ptr (null for trivially destructible)
    cc::function_ptr<void(void*)> deleter = nullptr;

    /// Node class index of the erased payload; used to return the slot to the right slab on free.
    cc::node_class_index class_index = {};

    // future: we have 7 padding bytes here (3 on wasm?)
};

/// A node_allocation for polymorphic types: the handle's static type may be a base while the live object is
/// any derived type in any size class. It keeps node_allocation<T>'s single-pointer handle, fast allocation
/// path, and stateless wait-free free; the price is that the size class can no longer be deduced from T, so
/// the user supplies a NodeTraits that destroys the object and reports the class it was allocated in. Use it
/// for intrusive polymorphic nodes (virtual or hand-rolled dispatch) without paying for a fat handle.
///
/// How it works: the handle stores only a T* (as node_allocation<T> does). At destruction it passes *ptr to
/// NodeTraits::destroy_and_get_class_index(T&), which must read the class index (typically via a virtual),
/// destroy the object, and return that index — read it before destroying, since the vtable is gone afterwards.
/// The slot is then returned to that class's slab. Move-only; copying is unsupported.
///
/// Example:
///     struct my_traits
///     {
///         static cc::node_class_index destroy_and_get_class_index(my_base& b)
///         {
///             auto const idx = b.node_class(); // virtual: the derived object's class index
///             b.~my_base();                    // virtual dtor: destroys the actual derived object
///             return idx;
///         }
///     };
///
///     auto* mem = alloc.allocate_node_bytes(
///         cc::node_class_index_for<my_derived>(), sizeof(my_derived), alignof(my_derived));
///     cc::poly_node_allocation<my_base, my_traits> h;
///     h.ptr = new (cc::placement_new, mem) my_derived();
template <class T, class NodeTraits>
struct cc::poly_node_allocation
{
    // properties
public:
    [[nodiscard]] bool is_valid() const { return ptr != nullptr; }
    explicit operator bool() const { return ptr != nullptr; }

    // smart pointer interface
public:
    [[nodiscard]] T& operator*() const
    {
        CC_ASSERT(ptr != nullptr, "dereferencing null poly_node_allocation");
        return *ptr;
    }
    [[nodiscard]] T* operator->() const
    {
        CC_ASSERT(ptr != nullptr, "dereferencing null poly_node_allocation");
        return ptr;
    }

    // ctors/dtor
public:
    poly_node_allocation() = default;

    poly_node_allocation(poly_node_allocation&& rhs) noexcept : ptr(cc::exchange(rhs.ptr, nullptr)) {}
    poly_node_allocation& operator=(poly_node_allocation&& rhs) noexcept
    {
        // take ownership from rhs while it's definitely still alive
        T* new_ptr = cc::exchange(rhs.ptr, nullptr);

        // now release current ownership
        reset();

        // and adopt the stolen pointer
        ptr = new_ptr;
        return *this;
    }
    poly_node_allocation(poly_node_allocation const&) = delete;
    poly_node_allocation& operator=(poly_node_allocation const&) = delete;

    /// Destroy the managed object and return its slot to the slab.
    /// Calls NodeTraits::destroy_and_get_class_index, then performs a single atomic_or into the slab's free bitmap.
    /// Wait-free and requires no allocator state; the slab base and slot index are derived from ptr.
    ~poly_node_allocation()
    {
        if (ptr != nullptr)
        {
            cc::node_class_index const class_idx = NodeTraits::destroy_and_get_class_index(*ptr);
            cc::node_allocation_free((cc::byte*)ptr, class_idx);
        }
    }

    /// Destroy the managed object (if any) and reset to empty state.
    /// Safe to call on an already-empty handle.
    void reset()
    {
        if (ptr != nullptr)
        {
            cc::node_class_index const class_idx = NodeTraits::destroy_and_get_class_index(*ptr);
            cc::node_allocation_free((cc::byte*)ptr, class_idx);
            ptr = nullptr;
        }
    }

    // members
public:
    /// Pointer to the managed T; nullptr indicates an empty handle.
    /// All deallocation metadata (slab base, slot index) is computed from this pointer.
    T* ptr = nullptr;
};
