#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/allocation_info.hh>
#include <shaped-graphics/types.hh>

/// Backend memory requirements for placing a resource into a memory_heap, returned by the memory_requirements_for_* queries.
/// Feed both fields to the external allocator that picks the offset.
struct sg::memory_requirements
{
    /// Required alignment in bytes, a power of two, for the resource's offset within the heap.
    /// The offset passed to acquire_allocation_for_* must be a multiple of it.
    isize alignment_in_bytes = 0;

    /// Size the resource actually occupies in the heap.
    /// May exceed the requested size, because of backend padding and alignment rules.
    isize size_in_bytes = 0;

    /// False where this heap cannot hold the resource at all, whatever the offset: a Vulkan image whose memory types exclude the heap's.
    /// The resource is then created dedicated instead.
    bool placeable = true;
};

/// A block of GPU memory into which many resources are sub-allocated, sharing one underlying allocation.
/// It is an immutable factory for allocation_info: the caller picks an offset, its own external allocator doing the sub-allocation and the tracking.
/// The heap validates that offset and mints an allocation_info pointing back into itself.
/// Held via memory_heap_handle, so a minted placement keeps the heap alive.
///
/// The intended flow: query the resource's requirements, your allocator picks an offset, `acquire_allocation_for_*(...)`.
/// Then pass the allocation_info to the matching create_* call.
///
/// Abstract: a backend subclasses it and owns the GPU allocation.
class sg::memory_heap : public std::enable_shared_from_this<memory_heap>
{
public:
    virtual ~memory_heap();

    /// Total size of this heap's underlying GPU allocation, in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Whether this heap places buffers or textures; a placement of the other kind asserts.
    [[nodiscard]] memory_heap_contents contents() const { return _contents; }

    /// Backend memory requirements for a buffer of this shape — the alignment its offset must satisfy, and the size it occupies.
    /// Query this first, since the external allocator uses it to pick a valid offset.
    [[nodiscard]] memory_requirements memory_requirements_for_buffer(isize size_in_bytes, buffer_usages usage) const;

    /// Mints a placement for a buffer of the given shape at `offset` within this heap.
    /// `offset` must be non-negative, aligned per memory_requirements_for_buffer, and leave room for the required size inside the heap.
    /// The placement's size is the backend-reported size, not the requested one, and the returned allocation_info holds a handle back to this heap.
    /// It does not track the allocation — the caller's external allocator owns that.
    [[nodiscard]] allocation_info acquire_allocation_for_buffer(isize size_in_bytes,
                                                                buffer_usages usage,
                                                                isize offset) const;

    /// Backend memory requirements for a texture of this description, in a `textures` heap.
    [[nodiscard]] memory_requirements memory_requirements_for_texture(texture_description const& desc) const;

    /// Mints a placement for a texture of this description at `offset` within this `textures` heap.
    /// `offset` must be aligned per memory_requirements_for_texture, leave room for its size, and the texture must be placeable here.
    /// A placed texture's contents are undefined until written, and so are those of every subresource it never writes.
    [[nodiscard]] allocation_info acquire_allocation_for_texture(texture_description const& desc, isize offset) const;

protected:
    explicit memory_heap(isize size_in_bytes, memory_heap_contents contents = memory_heap_contents::buffers);

    /// Backend hook: the memory requirements for a buffer of this shape.
    [[nodiscard]] virtual memory_requirements query_buffer_requirements(isize size_in_bytes, buffer_usages usage) const
        = 0;

    /// Backend hook: the memory requirements for a texture of this description.
    /// Only a backend that creates `textures` heaps is ever asked, which is why it is not pure.
    [[nodiscard]] virtual memory_requirements query_texture_requirements(texture_description const& desc) const;

    isize _size_in_bytes = 0;
    memory_heap_contents _contents = memory_heap_contents::buffers;
};
