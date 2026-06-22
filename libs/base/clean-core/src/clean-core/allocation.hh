#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/impl/object_lifetime_util.hh>
#include <clean-core/utility.hh>

// cc::allocation<T> is the owning “storage + liveness” handle underlying all contiguous heap containers.
//
// It models two things explicitly:
// 1) which bytes are owned (the allocation from a cc::memory_resource),
// 2) which objects inside those bytes are currently alive (the live window).
//
// Containers such as cc::array<T>, cc::vector<T>, and cc::devector<T> differ mainly in *policy*
// (how obj_start / obj_end move and when growth happens). The sharp, failure-prone mechanics—
// allocation ownership, resizing, alignment, and object lifetime—are centralized here.
//
// Memory is obtained from a polymorphic cc::memory_resource (POD, function-pointer based, static-init safe).
// The resource pointer is stored *in the allocation*, not as a template argument. A null resource
// means “use cc::default_memory_resource”. This avoids allocator-typed container variants, ABI bloat,
// and allocator-propagation complexity endemic to std-style allocator templates.
//
// Usage is simple by default: do nothing and the global default resource is used.
// Custom allocators are immediately supported: seed an empty allocation with a non-null resource,
// adopt an allocation from elsewhere, or reallocate across resources explicitly.
//
// Core invariants:
// - [alloc_start, alloc_end) is the owned byte range (exclusive end).
// - [obj_start, obj_end) is the live object range (exclusive end), always within the allocation.
// - obj_start and obj_end are always aligned to alignof(T), even when empty.
// - custom_resource == nullptr implies use of cc::default_memory_resource.
//
// Member layout (storage + metadata):
// - T* obj_start                         — pointer to the first live object
// - T* obj_end                           — one past the last live object
// - cc::byte* alloc_start                — base pointer returned by the memory resource
// - cc::byte* alloc_end                  — one past the last allocated byte
// - isize alignment                      — alignment used for allocate/deallocate of the byte block
// - cc::memory_resource const* custom_resource — owning resource, or nullptr for the global default

namespace cc
{
/// Default memory resource used when allocation::custom_resource == nullptr.
/// This is a system allocator stored in the data segment, making the pointer valid even during
/// static initialization in other translation units (safe for use in global/static constructors).
extern cc::memory_resource const* const default_memory_resource;
} // namespace cc

/// Polymorphic memory resource interface powering cc::allocation<T>.
/// Custom allocators implement this interface to provide pluggable allocation strategies.
/// The design favors explicit size/alignment tracking and non-movable in-place resize over realloc.
/// This is a POD struct using function pointers to avoid virtual dispatch and non-trivial constructors.
struct cc::memory_resource
{
    /// Allocate between `min_bytes` and `max_bytes` with at least `alignment` alignment.
    /// Returns the actual allocated size, which will be in [min_bytes, max_bytes].
    /// The allocated pointer is stored in `*out_ptr`.
    /// min_bytes == 0 always sets *out_ptr to nullptr and returns 0.
    /// min_bytes > 0 always sets *out_ptr to non-null; failure is fatal (assert/terminate) or throws.
    /// Allocators that round to size classes can report the rounded-up size to allow more effective memory use.
    /// REQUIRED: Must be non-null for valid memory resources.
    cc::function_ptr<isize(cc::byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)> allocate_bytes
        = nullptr;

    /// Attempt to allocate between `min_bytes` and `max_bytes` with at least `alignment` alignment.
    /// Returns the actual allocated size on success, or -1 on failure.
    /// The allocated pointer is stored in `*out_ptr` on success, or nullptr on failure.
    /// min_bytes == 0 always sets *out_ptr to nullptr and returns 0.
    /// min_bytes > 0 may return -1 and set *out_ptr to nullptr to signal allocation was not possible.
    /// This provides an escape hatch for callers that must handle allocation failure explicitly.
    /// Implementations should prefer returning -1 over fatal failure when feasible (best-effort).
    /// Wrappers are still permitted to fatally fail rather than return -1.
    /// Allocators that round to size classes can report the rounded-up size to allow more effective memory use.
    /// OPTIONAL: May be nullptr; resources that don't support fallible allocation can leave this unset.
    cc::function_ptr<isize(cc::byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)> try_allocate_bytes
        = nullptr;

    /// Deallocate a block previously obtained from this resource with matching bytes and alignment.
    /// `p` must be the exact pointer returned by allocate_bytes or try_allocate_bytes.
    /// `bytes` and `alignment` must match the values used during allocation.
    /// Noexcept in spirit: only programmer bugs (e.g., mismatched size) may throw or terminate.
    /// Allocator exhaustion itself must not throw; containers may leak memory if this throws.
    /// REQUIRED: Must be non-null for valid memory resources.
    cc::function_ptr<void(cc::byte* p, isize bytes, isize alignment, void* userdata)> deallocate_bytes = nullptr;

    /// Attempt to resize an existing allocation in place without moving or freeing it.
    /// This optimization hook enables contiguous containers to grow/shrink without invalidating iterators.
    /// The resize range [min_bytes, max_bytes] lets the resource choose any size fitting internal constraints.
    ///
    /// Preconditions:
    /// `p` was allocated from this resource with `old_bytes` and `alignment`.
    /// `1 <= min_bytes <= max_bytes`.
    ///
    /// Success (returns new_bytes in [min_bytes, max_bytes]):
    /// The allocation remains at address `p` (no move).
    /// The first min(old_bytes, new_bytes) bytes are preserved.
    /// The returned size becomes the canonical size for future resize/deallocate calls.
    ///
    /// Failure (returns -1):
    /// The allocation remains valid and unchanged at `p` with size `old_bytes`.
    /// Ownership remains with the caller; the resource does not free or invalidate `p`.
    ///
    /// Supports both growth (min_bytes > old_bytes) and shrink (max_bytes < old_bytes).
    /// Shrink success does not guarantee memory returned to OS; only updates logical allocation size.
    /// Prefer returning the smallest representable size >= min_bytes to minimize memory waste.
    /// `alignment` cannot be increased; it matches the original allocation.
    ///
    /// Rationale: Traditional realloc may move and implicitly free the original allocation.
    /// This is unsafe for containers where element addresses must remain stable during reallocation.
    /// Example: vector::push_back(vec[0]) where the source aliases the container's storage.
    ///
    /// Noexcept in spirit: only programmer bugs (e.g., mismatched old_bytes) may throw or terminate.
    /// Allocator exhaustion itself must not throw; containers may leak memory if this throws.
    ///
    /// OPTIONAL: May be nullptr; resources that don't support in-place resize can leave this unset.
    cc::function_ptr<isize(cc::byte* p, isize old_bytes, isize min_bytes, isize max_bytes, isize alignment, void* userdata)>
        try_resize_bytes_in_place = nullptr;

    /// User-defined data for custom allocators. Can be nullptr for stateless allocators.
    void* userdata = nullptr;
};

/// Owning allocation handle for a contiguous byte block plus a typed "live window" inside it.
///
/// Design goals:
/// - Separate "what memory do we own?" from "which objects are currently alive in it?".
/// - Support pooling / reuse without reallocating by keeping the underlying byte allocation intact.
/// - Enable zero-copy interop between contiguous owning containers (adopt/release), and safe fallible
///   re-interpretation (retype) for trivially copyable payloads when the live window is cleared.
///
/// This is intentionally more expressive than the classic (ptr, size, capacity) triple:
/// - We keep the original allocation bounds (alloc_start/alloc_end) so we can round-trip capacity
///   across retypes and support realignment by moving obj_start/obj_end within the allocation.
/// - We treat capacity as an implicit property derived from the remaining bytes between obj_end and
///   alloc_end (and, for front-growth containers, between alloc_start and obj_start).
///
/// Philosophy:
/// - All owning containers that store their elements contiguously should be built on cc::allocation<T>.
///   This includes the main containers: array<T>, vector<T>, and devector<T> (double-ended vector).
/// - Sharing this handle gives those containers a uniform "escape hatch" story: allocations can be
///   adopted, released, retyped, and transferred across container types without copying.
/// - Non-owning views (span<T> / fixed_span<T, N>) are the common interop surface: contiguous containers
///   can usually expose span<T> views of their live window.
///
/// Fixed-size container variants:
/// - fixed_* containers carry their extent/capacity as template argument N and store data inline.
/// - They reuse the same object-lifetime helpers (construct/destroy/commit patterns) but do not use
///   cc::allocation<T> since there is no heap allocation to adopt/release; copying/moving is explicit.
/// - fixed_array<T, N> is fully alive and can provide fixed_span<T, N> in addition to span<T>.
///   fixed_vector<T, N> and fixed_devector<T, N> track partial liveness but still remain inline.
///
/// Tradeoffs:
/// - This handle is larger than a minimal vector header, but it centralizes allocation metadata that
///   we need for robust pooling/reuse, deallocation correctness, and strong container interop.
/// - Ring buffers with wrap-around are intentionally not represented here: once data wraps, the live
///   region becomes segmented and no longer matches contiguous container semantics.
/// - Alignment above alignof(T) is supported by moving the live window within the byte allocation
///   (usually after clear), rather than by infecting container types with alignment template args.
///
/// Invariants (unless stated otherwise by a specific container):
/// - [obj_start, obj_end) is the live object range; obj_end is exclusive.
/// - size() is (obj_end - obj_start) in elements.
/// - [alloc_start, alloc_end) is the owned byte allocation; alloc_end is exclusive.
/// - alloc_start <= obj_start <= obj_end <= alloc_end (even for empty ranges or empty allocations).
/// - obj_start and obj_end must be aligned to alignof(T) (even when the range is empty).
/// - resource == nullptr means the global default memory resource is used.
template <class T>
struct cc::allocation
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "allocations need to refer to non-const objects, not references/functions/void");

    /// Pointer to the first live object.
    ///
    /// Points into the owned byte allocation. The live range is contiguous and begins here.
    /// The object lifetime model is: all objects in [obj_start, obj_end) are alive; outside it is
    /// dead storage. For vector-like containers, obj_start is typically also the "data()" pointer.
    ///
    /// INVARIANT: Must always be aligned to alignof(T), even if the range is empty.
    T* obj_start = nullptr;

    /// Pointer one past the last live object (exclusive end).
    ///
    /// This is the classic half-open range convention: [obj_start, obj_end).
    /// The number of live elements is (obj_end - obj_start).
    ///
    /// INVARIANT: Must always be aligned to alignof(T), even if the range is empty.
    T* obj_end = nullptr;

    /// Start of the owned byte allocation (base pointer returned by the memory resource).
    ///
    /// This pointer must be passed back to the memory resource for deallocation.
    /// We keep the original allocation bounds so we can:
    /// - compute implicit capacity in bytes/elements,
    /// - preserve the underlying byte capacity across retypes,
    /// - and support realignment by moving obj_start/obj_end forward within the allocation.
    cc::byte* alloc_start = nullptr;

    /// End of the owned byte allocation (exclusive).
    ///
    /// The owned byte range is [alloc_start, alloc_end). The total allocated size in bytes is
    /// (alloc_end - alloc_start). This may be larger than the bytes "currently usable" for T if
    /// obj_start was aligned forward for a stricter alignment during reuse/retype.
    cc::byte* alloc_end = nullptr;

    /// Requested alignment for the owned byte allocation (in bytes).
    ///
    /// This is the alignment that was used when allocating [alloc_start, alloc_end) from the resource.
    /// It is stored to enable correct deallocation for resources that require the original alignment
    /// (and for debugging/validation). Note that obj_start may be aligned further forward than this
    /// when retyping to a type U with larger alignof(U); in that case the live window shifts, while
    /// the underlying allocation alignment metadata remains unchanged.
    isize alignment = 0;

    /// Memory resource that owns the allocation, or nullptr for the global default.
    ///
    /// Null means "use global fallback". This makes the all-zero state a valid empty allocation:
    /// - no owned bytes,
    /// - no live objects,
    /// - and the default resource implied.
    ///
    /// Containers can select a non-default resource without cluttering APIs by seeding an empty
    /// allocation that carries the desired resource; subsequent growth/reallocation uses that resource.
    cc::memory_resource const* custom_resource = nullptr;

    // minimal helper api
public:
    /// Returns the effective resource to use for allocation operations.
    /// Resolves custom_resource if non-null, otherwise falls back to default_memory_resource.
    [[nodiscard]] cc::memory_resource const& resource() const
    {
        return custom_resource ? *custom_resource : *default_memory_resource;
    }

    /// True iff this is a valid non-defaulted allocation
    /// Implies byte size > 0, i.e. alloc_start < alloc_end
    /// But obj_span might still be empty
    [[nodiscard]] bool is_valid() const { return alloc_start != nullptr; }

    /// Returns the span of live objects
    /// Note: proper mutability ("const correctness") is user responsibility
    [[nodiscard]] cc::span<T> obj_span() const { return cc::span<T>(obj_start, obj_end); }

    /// Number of allocated bytes
    [[nodiscard]] isize alloc_size_bytes() const { return alloc_end - alloc_start; }

    /// Attempt to resize the allocation in place to a size between min_bytes and max_bytes.
    /// Returns true if the resize succeeded, false otherwise.
    /// On success, alloc_end is updated to reflect the new allocation size.
    /// On failure, the allocation remains unchanged.
    /// IMPORTANT: Cannot resize below the size needed by live objects (obj_end).
    /// NOTE: Can be used for both growing and shrinking. Does not check if the allocation
    /// is already within [min_bytes, max_bytes]; the caller should check this if desired.
    [[nodiscard]] bool try_resize_alloc_inplace(isize min_bytes, isize max_bytes)
    {
        CC_ASSERT(min_bytes >= 0 && max_bytes >= min_bytes, "try_resize_alloc_inplace: invalid size range");

        // Cannot resize below the memory occupied by live objects
        isize const obj_end_bytes = (byte const*)obj_end - alloc_start;
        CC_ASSERT(min_bytes >= obj_end_bytes, "try_resize_alloc_inplace: cannot resize below live object range");

        // If no allocation exists, cannot resize
        if (alloc_start == nullptr)
            return false;

        auto const old_bytes = alloc_end - alloc_start;
        auto const& res = resource();

        // no support for resizing?
        if (res.try_resize_bytes_in_place == nullptr)
            return false;

        // Try to resize in place using the allocator API
        isize const new_bytes
            = res.try_resize_bytes_in_place(alloc_start, old_bytes, min_bytes, max_bytes, alignment, res.userdata);

        // Check if resize failed
        if (new_bytes == -1)
            return false;

        // Success: update alloc_end to reflect the new size
        alloc_end = alloc_start + new_bytes;
        return true;
    }

    /// Resize the allocation to a size between min_bytes and max_bytes with a new alignment.
    /// Always tries to resize in-place first. If that fails, allocates a new buffer,
    /// moves the live objects over, and replaces the current allocation.
    /// IMPORTANT: Cannot resize below the size needed by live objects (obj_end).
    /// NOTE: Can be used for both growing and shrinking. Does not check if the allocation
    /// is already within [min_bytes, max_bytes]; the caller should check this if desired.
    void resize_alloc(isize min_bytes, isize max_bytes, isize new_alignment)
    {
        CC_ASSERT(min_bytes >= 0 && max_bytes >= min_bytes, "resize_alloc: invalid size range");
        CC_ASSERT(new_alignment >= alignof(T), "new_alignment must be at least alignof(T)");

        // Cannot resize below the memory occupied by live objects
        isize const obj_end_bytes = (byte const*)obj_end - alloc_start;
        CC_ASSERT(min_bytes >= obj_end_bytes, "resize_alloc: cannot resize below live object range");

        // Try to resize in-place first if current allocation already satisfies new alignment
        if (cc::is_aligned(alloc_start, new_alignment) && try_resize_alloc_inplace(min_bytes, max_bytes))
        {
            // Write through the new alignment (in-place resize doesn't change it)
            alignment = new_alignment;
            return;
        }

        // In-place resize failed or alignment requirement not satisfied, allocate a new buffer
        auto new_alloc = allocation::create_empty_bytes(min_bytes, max_bytes, new_alignment, custom_resource);

        // Move-create live objects to the new allocation
        impl::move_create_objects_to(new_alloc.obj_end, obj_start, obj_end);

        // Move the new allocation to *this (this destroys the old allocation)
        *this = cc::move(new_alloc);
    }

    // factories
public:
    /// Creates an empty allocation with reserved capacity but no live objects.
    ///
    /// Allocates between min_bytes and max_bytes with the specified alignment, but does not construct any objects.
    /// The result has obj_start == obj_end == alloc_start (zero live objects, full capacity available).
    /// This is useful for containers that want to reserve memory upfront and construct objects incrementally.
    ///
    /// Allocators will typically return min_bytes, but may return more (up to max_bytes) if they've
    /// internally rounded up to a larger size class, avoiding waste.
    /// The alignment parameter allows over-alignment beyond alignof(T).
    /// min_bytes == 0 results in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_empty_bytes(isize min_bytes,
                                                       isize max_bytes, // NOLINT
                                                       isize alignment, // NOLINT
                                                       memory_resource const* resource,
                                                       isize obj_offset = 0)
    {
        CC_ASSERT(alignment >= alignof(T), "alignment must be at least alignof(T)");
        CC_ASSERT(0 <= min_bytes && min_bytes <= max_bytes, "must have 0 <= min_bytes <= max_bytes");
        CC_ASSERT(obj_offset * isize(sizeof(T)) <= min_bytes, "obj_offset would result in invalid allocation");

        allocation result;
        result.custom_resource = resource;
        result.alignment = alignment;

        // Resolve the actual resource to use
        auto const& res = resource ? *resource : *default_memory_resource;

        // Allocate bytes (even if zero-sized)
        auto const actual_byte_size
            = res.allocate_bytes(&result.alloc_start, min_bytes, max_bytes, result.alignment, res.userdata);
        result.alloc_end = result.alloc_start + actual_byte_size;

        // Initialize obj_start and obj_end to alloc_start + obj_offset (zero live objects, full capacity)
        result.obj_start = (T*)result.alloc_start + obj_offset;
        result.obj_end = result.obj_start;

        return result;
    }

    /// Creates an empty allocation with reserved capacity but no live objects.
    ///
    /// Allocates space for 'size' objects with the specified alignment, but does not construct any objects.
    /// The result has obj_start == obj_end == alloc_start (zero live objects, full capacity available).
    /// This is useful for containers that want to reserve memory upfront and construct objects incrementally.
    ///
    /// The alignment parameter allows over-alignment beyond alignof(T).
    /// size == 0 results in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_empty(isize size, isize alignment, memory_resource const* resource) // NOLINT
    {
        auto const min_byte_size = size * sizeof(T);
        return create_empty_bytes(min_byte_size, min_byte_size, alignment, resource);
    }

    /// Creates an allocation with a specified count of default-constructed objects.
    ///
    /// Allocates space for 'size' objects and default-constructs all of them.
    /// The result is a "tight" allocation: allocated bytes exactly match live object count,
    /// with obj_start and obj_end spanning the full allocated range (no spare capacity).
    ///
    /// Objects are default-constructed via default_create_objects_to, which uses T().
    /// This ensures zero-initialization for primitive types (e.g., int, float, pointers).
    /// Alignment is set to alignof(T).
    ///
    /// Empty allocations (size == 0) result in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_defaulted(isize size, memory_resource const* resource)
    {
        auto result = allocation::create_empty(size, alignof(T), resource);
        impl::default_create_objects_to(result.obj_end, size);
        return result;
    }

    /// Creates an allocation with a specified count of objects, all copy-constructed from a single value.
    ///
    /// Allocates space for 'size' objects and copy-constructs all of them from 'value'.
    /// The result is a "tight" allocation: allocated bytes exactly match live object count,
    /// with obj_start and obj_end spanning the full allocated range (no spare capacity).
    ///
    /// Objects are copy-constructed via fill_create_objects_to.
    /// Alignment is set to alignof(T).
    ///
    /// Empty allocations (size == 0) result in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_filled(isize size, T const& value, memory_resource const* resource)
    {
        auto result = allocation::create_empty(size, alignof(T), resource);
        impl::fill_create_objects_to(result.obj_end, size, value);
        return result;
    }

    /// Creates an allocation with uninitialized memory treated as live objects.
    ///
    /// Allocates space for 'size' objects but does NOT construct them. Instead, obj_end is set to
    /// the end of the allocation, treating the uninitialized memory as if it contains live objects.
    /// The result is a "tight" allocation: allocated bytes exactly match the live object range,
    /// with obj_start and obj_end spanning the full allocated range (no spare capacity).
    ///
    /// IMPORTANT: This is only safe for trivially copyable and trivially destructible types,
    /// as enforced by static assertions. The caller is responsible for properly initializing
    /// the memory before reading from it (e.g., via memcpy or direct writes).
    ///
    /// Alignment is set to alignof(T).
    /// Empty allocations (size == 0) result in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_uninitialized(isize size, memory_resource const* resource)
    {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for uninitialized allocation");
        static_assert(std::is_trivially_destructible_v<T>, "T must be trivially destructible for uninitialized "
                                                           "allocation");

        auto result = allocation::create_empty(size, alignof(T), resource);
        result.obj_end = result.obj_start + size;
        return result;
    }

    /// Creates an allocation with uninitialized memory treated as live objects (UNSAFE).
    ///
    /// Allocates space for 'size' objects but does NOT construct them. Instead, obj_end is set to
    /// the end of the allocation, treating the uninitialized memory as if it contains live objects.
    /// The result is a "tight" allocation: allocated bytes exactly match the live object range,
    /// with obj_start and obj_end spanning the full allocated range (no spare capacity).
    ///
    /// DANGEROUS: Unlike create_uninitialized, this version does NOT enforce trivial copyability
    /// or trivial destructibility via static assertions. The caller MUST ensure that:
    /// 1. The type has a trivial destructor (or the memory is properly initialized before destruction)
    /// 2. The memory is properly initialized before any reads or operations that assume constructed objects
    ///
    /// Only use this when you need uninitialized allocation for types that don't satisfy the
    /// create_uninitialized requirements but you can guarantee safety through other means.
    ///
    /// Alignment is set to alignof(T).
    /// Empty allocations (size == 0) result in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_uninitialized_unsafe(isize size, memory_resource const* resource)
    {
        auto result = allocation::create_empty(size, alignof(T), resource);
        result.obj_end = result.obj_start + size;
        return result;
    }

    /// Creates a deep copy of a span of objects using the specified memory resource.
    ///
    /// Copies all objects from the provided span.
    /// The result is a "tight" allocation: allocated bytes exactly match live object count,
    /// with obj_start and obj_end spanning the full allocated range (no spare capacity).
    ///
    /// Objects are copy-constructed via copy_create_objects_to.
    /// Alignment is set to alignof(T).
    ///
    /// Empty spans (size == 0) result in nullptr with no real allocation call.
    [[nodiscard]] static allocation create_copy_of(span<T const> source, memory_resource const* resource)
    {
        auto result = allocation::create_empty(source.size(), alignof(T), resource);
        impl::copy_create_objects_to(result.obj_end, source.data(), source.data() + source.size());
        return result;
    }

    /// Same as create_copy_of(source, resource) but uses the default memory resource
    [[nodiscard]] static allocation create_copy_of(span<T const> source)
    {
        return allocation::create_copy_of(source, nullptr);
    }

    /// Creates a deep copy of another allocation using the specified memory resource.
    ///
    /// Copies only the live object range [rhs.obj_start, rhs.obj_end), not the full capacity.
    /// This is a convenience overload that forwards to create_copy_of(span, resource).
    ///
    /// The resource parameter may differ from rhs.custom_resource, enabling cross-resource copies.
    [[nodiscard]] static allocation create_copy_of(allocation const& rhs, memory_resource const* resource)
    {
        return allocation::create_copy_of(rhs.obj_span(), resource);
    }

    /// Same as create_copy_of(rhs, resource) but uses the same memory resource as rhs
    [[nodiscard]] static allocation create_copy_of(allocation const& rhs)
    {
        return allocation::create_copy_of(rhs.obj_span(), rhs.custom_resource);
    }

    // lifecycle
public:
    allocation() = default;

    // no implicit copies for allocations
    // downstream containers need to handle this explicitly!
    allocation(allocation const&) = delete;
    allocation& operator=(allocation const&) = delete;

    allocation(allocation&& rhs) noexcept
      : obj_start(cc::exchange(rhs.obj_start, nullptr)),
        obj_end(cc::exchange(rhs.obj_end, nullptr)),
        alloc_start(cc::exchange(rhs.alloc_start, nullptr)),
        alloc_end(cc::exchange(rhs.alloc_end, nullptr)),
        alignment(cc::exchange(rhs.alignment, 0)),
        custom_resource(rhs.custom_resource) // rhs resource stays
    {
    }

    /// Move assignment operator with nested-rhs safety guarantee.
    ///
    /// This implementation is safe even when rhs is nested inside one of the objects
    /// being destroyed in 'this'. The critical ordering is:
    ///
    /// 1. Move rhs into a local temporary (which clears rhs via move constructor)
    /// 2. Destroy objects in 'this' (safe even if this destroys rhs, since it's already cleared)
    /// 3. Transfer ownership from the temporary to 'this'
    ///
    /// This ensures that if rhs is destroyed during step 2, it's already been moved-from
    /// and won't attempt to deallocate its memory (preventing double-free).
    ///
    /// Example scenario this protects against:
    ///   allocation<SomeStruct> outer;
    ///   outer contains: SomeStruct{ allocation<SomeStruct> inner; }
    ///   outer = std::move(outer[0].inner);  // rhs nested in 'this'
    ///
    allocation& operator=(allocation&& rhs) noexcept
    {
        if (this != &rhs)
        {
            // Move rhs into temporary - this clears rhs via the move constructor
            auto rhs_tmp = cc::move(rhs);

            // Destroy existing objects and deallocate
            impl::destroy_objects_in_reverse(obj_start, obj_end);
            if (alloc_start != nullptr)
            {
                auto const& res = resource();
                res.deallocate_bytes(alloc_start, alloc_end - alloc_start, alignment, res.userdata);
            }

            // Transfer ownership from tmp
            obj_start = cc::exchange(rhs_tmp.obj_start, nullptr);
            obj_end = cc::exchange(rhs_tmp.obj_end, nullptr);
            alloc_start = cc::exchange(rhs_tmp.alloc_start, nullptr);
            alloc_end = cc::exchange(rhs_tmp.alloc_end, nullptr);
            alignment = cc::exchange(rhs_tmp.alignment, 0);
            custom_resource = rhs_tmp.custom_resource; // rhs resource stays
        }

        return *this;
    }

    ~allocation()
    {
        // end life and call dtor of live objects
        impl::destroy_objects_in_reverse(obj_start, obj_end);

        // return allocation
        if (alloc_start != nullptr)
        {
            auto const& res = resource();
            res.deallocate_bytes(alloc_start, alloc_end - alloc_start, alignment, res.userdata);
        }
    }
};
