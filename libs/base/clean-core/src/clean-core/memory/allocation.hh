#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/impl/object_lifetime_util.hh>

// cc::allocation<T> is the owning storage handle underneath every contiguous heap container.
// It tracks two ranges independently: the bytes owned from a cc::memory_resource, and the live objects inside them.
// cc::array<T> and cc::vector<T> differ only in policy — how obj_start / obj_end move, and when growth happens.
//
// The model, the resource interface, the member meanings and the current gaps are in libs/base/clean-core/docs/systems/allocation.md.
//
// Two things worth knowing before reading further.
// The resource pointer is stored in the handle rather than as a template argument, which is the escape from allocator-typed container variants and allocator-propagation rules.
// A null resource means cc::default_memory_resource, which is what makes the all-zero state a valid empty allocation.

namespace cc
{
/// Default memory resource used when allocation::custom_resource == nullptr.
/// What backs it is a build choice, and CC_HAS_MIMALLOC is the one that says which: mimalloc under 1
/// (memory/mimalloc_resource.cc, our fast general-purpose allocator), and cc::system_memory_resource itself under 0.
/// So the two are the same object in a CC_HAS_MIMALLOC=0 build, which is what the sanitize presets configure —
/// see docs/platforms.md, "Default allocator (SC_MIMALLOC)".
/// Stored in the data segment, making the pointer valid even during static initialization in
/// other translation units (safe for use in global/static constructors).
extern cc::memory_resource const* const default_memory_resource;

/// The platform malloc/free resource (_aligned_malloc / posix_memalign + free), data-segment resident like the default.
/// Passing &cc::system_memory_resource as a custom resource is always well defined; where mimalloc backs the default it
/// is the explicit opt-out from it, and where it does not there is nothing to opt out of.
extern cc::memory_resource const system_memory_resource;
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
    /// Returns the actual allocated size, or -1 on failure; `*out_ptr` holds the pointer on success and nullptr on failure.
    /// min_bytes == 0 always sets *out_ptr to nullptr and returns 0.
    /// This is the escape hatch for callers that must handle allocation failure explicitly, so prefer returning -1 over failing fatally where feasible.
    /// A wrapper is still permitted to fail fatally instead.
    /// An allocator that rounds to size classes can report the rounded-up size, so the caller gets the extra capacity for free.
    /// OPTIONAL: may be nullptr when a resource does not support fallible allocation.
    cc::function_ptr<isize(cc::byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)> try_allocate_bytes
        = nullptr;

    /// Deallocate a block previously obtained from this resource with matching bytes and alignment.
    /// `p` must be the exact pointer returned by allocate_bytes or try_allocate_bytes.
    /// `bytes` and `alignment` must match the values used during allocation.
    /// Noexcept in spirit: only programmer bugs (e.g., mismatched size) may throw or terminate.
    /// Allocator exhaustion itself must not throw; containers may leak memory if this throws.
    /// REQUIRED: Must be non-null for valid memory resources.
    cc::function_ptr<void(cc::byte* p, isize bytes, isize alignment, void* userdata)> deallocate_bytes = nullptr;

    /// Attempt to resize an existing allocation in place, without moving or freeing it.
    /// The [min_bytes, max_bytes] range lets the resource pick any size that fits its internal constraints.
    ///
    /// `p` must have come from this resource with `old_bytes` and `alignment`, and 1 <= min_bytes <= max_bytes must hold.
    /// `alignment` cannot be raised; it matches the original allocation.
    ///
    /// On success it returns the new size in [min_bytes, max_bytes], the block stays at `p`, and the first min(old_bytes, new_bytes) bytes are preserved.
    /// The returned size becomes the canonical size for later resize / deallocate calls.
    /// On failure it returns -1 and leaves the allocation valid and unchanged at `p` with size `old_bytes`; ownership never transfers.
    ///
    /// Growth and shrink are both supported, though a successful shrink only updates the logical size and need not return memory to the OS.
    /// Prefer the smallest representable size >= min_bytes, to minimize waste.
    ///
    /// This exists instead of realloc because realloc may move the block and implicitly free the original.
    /// That is unsafe when element addresses must stay stable across a reallocation — vector::push_back(vec[0]), where the source aliases the container's own storage.
    ///
    /// Noexcept in spirit: only programmer bugs, such as a mismatched old_bytes, may throw or terminate.
    /// Allocator exhaustion itself must not throw, and a container may leak memory if it does.
    ///
    /// OPTIONAL: may be nullptr when a resource does not support in-place resize.
    cc::function_ptr<isize(cc::byte* p, isize old_bytes, isize min_bytes, isize max_bytes, isize alignment, void* userdata)>
        try_resize_bytes_in_place = nullptr;

    /// User-defined data for custom allocators.
    /// May be nullptr for a stateless allocator.
    void* userdata = nullptr;
};

/// Owning allocation handle: a contiguous byte block plus a typed live window inside it.
///
/// The design rationale, the container policies built on it, and the known gaps are in libs/base/clean-core/docs/systems/allocation.md.
///
/// Invariants, unless a specific container states otherwise:
/// - [obj_start, obj_end) is the live object range, obj_end exclusive; size() is (obj_end - obj_start) elements.
/// - [alloc_start, alloc_end) is the owned byte allocation, alloc_end exclusive.
/// - alloc_start <= obj_start <= obj_end <= alloc_end, even for empty ranges and empty allocations.
/// - obj_start and obj_end are aligned to alignof(T), even when the range is empty.
/// - custom_resource == nullptr means the global default memory resource.
///
/// Ring buffers with wrap-around are deliberately not representable here: once data wraps, the live region is segmented and no longer one contiguous window.
/// cc::ringbuffer therefore holds an allocation as a pure byte handle, keeping obj_start == obj_end and managing its elements' lifetimes itself.
template <class T>
struct cc::allocation
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    /// Pointer to the first live object, and typically a container's data().
    /// Everything outside [obj_start, obj_end) is dead storage.
    ///
    /// INVARIANT: always aligned to alignof(T), even when the range is empty.
    T* obj_start = nullptr;

    /// One past the last live object (exclusive); the element count is (obj_end - obj_start).
    ///
    /// INVARIANT: always aligned to alignof(T), even when the range is empty.
    T* obj_end = nullptr;

    /// Base pointer of the owned byte allocation, and the pointer that must be handed back to the resource to deallocate.
    cc::byte* alloc_start = nullptr;

    /// One past the owned bytes (exclusive); the allocated size is (alloc_end - alloc_start).
    /// May exceed the bytes currently usable for T, when obj_start was aligned forward for a stricter alignment.
    cc::byte* alloc_end = nullptr;

    /// Alignment that [alloc_start, alloc_end) was allocated with, kept because some resources need it back to deallocate correctly.
    /// obj_start may sit further forward than this after retyping to a stricter-aligned U; the allocation's own alignment does not change.
    isize alignment = 0;

    /// Memory resource that owns the allocation, or nullptr for the global default.
    /// Null is what makes the all-zero state a valid empty allocation: no owned bytes, no live objects, default resource implied.
    /// A container selects a non-default resource by seeding an empty allocation that carries it, and later growth uses that resource.
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

    /// Attempts to resize the byte allocation in place to a size in [min_bytes, max_bytes], updating alloc_end on success.
    /// Returns false and leaves the allocation unchanged otherwise.
    /// min_bytes must not fall below the bytes the live objects already occupy.
    /// Grows and shrinks both, and does not check whether the allocation already fits the range — check first if that matters.
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

    /// Resizes the byte allocation to a size in [min_bytes, max_bytes] under a new alignment.
    /// new_alignment must be at least alignof(T), and min_bytes must not fall below the bytes the live objects already occupy.
    /// In place is tried first, but only when the current block already satisfies new_alignment; a raise in alignment always reallocates and move-constructs the live objects over.
    /// Grows and shrinks both, and does not check whether the allocation already fits the range — check first if that matters.
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

    /// Creates an allocation whose bytes are treated as live objects without constructing them.
    ///
    /// obj_end is set to the end of the allocation, so the result is tight: the allocated bytes exactly match the live range, with no spare capacity.
    /// The caller must initialize the memory before reading it, e.g. via memcpy or direct writes.
    /// T must be trivially copyable and trivially destructible, static-asserted below.
    ///
    /// Alignment is alignof(T), and size == 0 yields nullptr with no real allocation call.
    [[nodiscard]] static allocation create_uninitialized(isize size, memory_resource const* resource)
    {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for uninitialized allocation");
        static_assert(std::is_trivially_destructible_v<T>, "T must be trivially destructible for uninitialized "
                                                           "allocation");

        auto result = allocation::create_empty(size, alignof(T), resource);
        result.obj_end = result.obj_start + size;
        return result;
    }

    /// Creates an allocation whose bytes are treated as live objects without constructing them, and without the safety checks.
    ///
    /// The result is as tight as create_uninitialized's, but nothing static-asserts trivial copyability or trivial destructibility.
    /// The caller must guarantee a trivial destructor, or initialization before destruction, and initialization before any read.
    /// Reach for this only when a type cannot satisfy create_uninitialized's requirements and safety is guaranteed some other way.
    ///
    /// Alignment is alignof(T), and size == 0 yields nullptr with no real allocation call.
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
