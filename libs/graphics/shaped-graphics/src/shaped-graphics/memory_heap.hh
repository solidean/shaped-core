#pragma once

#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// Backend memory requirements for placing a resource into a memory_heap. Returned by the
/// memory_requirements_for_* queries; feed both fields to the external allocator that picks the offset.
struct memory_requirements
{
    /// Required alignment (bytes, a power of two) for the resource's offset within the heap. The offset
    /// passed to acquire_allocation_for_* must be a multiple of this.
    isize alignment_in_bytes = 0;

    /// Size the resource actually occupies in the heap — may exceed the requested size due to backend
    /// padding/alignment rules.
    isize size_in_bytes = 0;
};

/// A block of GPU memory into which many resources are sub-allocated, sharing one underlying allocation.
/// It is an immutable factory for allocation_info: the caller picks an offset (its own external allocator
/// does the sub-allocation and tracking), and the heap validates that offset and mints an allocation_info
/// pointing back into itself. Held via memory_heap_handle so a minted placement keeps the heap alive.
///
/// Intended flow: query the resource's requirements → your allocator picks an offset → heap
/// acquire_allocation_for_*(...) → pass the allocation_info to the matching create_* call.
///
/// Abstract: a backend subclasses it and owns the GPU allocation.
class memory_heap : public std::enable_shared_from_this<memory_heap>
{
public:
    virtual ~memory_heap();

    /// Total size of this heap's underlying GPU allocation, in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Backend memory requirements for a buffer of this shape — the alignment its offset must satisfy and
    /// the size it occupies. Query this first; the external allocator uses it to pick a valid offset.
    [[nodiscard]] memory_requirements memory_requirements_for_buffer(isize size_in_bytes, buffer_usage usage) const;

    /// Mints a placement for a buffer of the given shape at `offset` within this heap. `offset` must be
    /// non-negative, aligned per memory_requirements_for_buffer, and leave room for the required size
    /// inside the heap. The placement's size is the backend-reported size, not the requested one. The
    /// returned allocation_info holds a handle back to this heap. Does not track the allocation — the
    /// caller's external allocator owns that.
    [[nodiscard]] allocation_info acquire_allocation_for_buffer(isize size_in_bytes, buffer_usage usage, isize offset) const;

protected:
    explicit memory_heap(isize size_in_bytes);

    /// Backend hook: memory requirements (alignment + occupied size) for a buffer of this shape.
    [[nodiscard]] virtual memory_requirements query_buffer_requirements(isize size_in_bytes, buffer_usage usage) const
        = 0;

    isize _size_in_bytes = 0;
};
} // namespace sg
