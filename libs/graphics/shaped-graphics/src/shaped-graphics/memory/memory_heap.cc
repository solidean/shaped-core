#include <clean-core/common/assert.hh>
#include <shaped-graphics/memory/memory_heap.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg
{
memory_heap::~memory_heap() = default;

memory_heap::memory_heap(isize size_in_bytes, memory_heap_contents contents)
  : _size_in_bytes(size_in_bytes), _contents(contents)
{
    // Zero is allowed — an empty heap holds no placements.
    CC_ASSERT(size_in_bytes >= 0, "memory heap size must be non-negative");
}

memory_requirements memory_heap::memory_requirements_for_buffer(isize size_in_bytes, buffer_usages usage) const
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    CC_ASSERT(_contents == memory_heap_contents::buffers, "a textures heap places no buffers");
    return query_buffer_requirements(size_in_bytes, usage);
}

memory_requirements memory_heap::memory_requirements_for_texture(texture_description const& desc) const
{
    CC_ASSERT(_contents == memory_heap_contents::textures, "a buffers heap places no textures");
    return query_texture_requirements(desc);
}

memory_requirements memory_heap::query_texture_requirements(texture_description const&) const
{
    CC_ASSERT(false, "this backend created a textures heap it cannot place textures in");
    return {.placeable = false};
}

allocation_info memory_heap::acquire_allocation_for_texture(texture_description const& desc, isize offset) const
{
    memory_requirements const reqs = memory_requirements_for_texture(desc);
    CC_ASSERT(reqs.placeable, "the texture cannot be placed in this heap");
    CC_ASSERT(reqs.alignment_in_bytes > 0, "backend must report a positive alignment");
    CC_ASSERT(offset >= 0 && offset % reqs.alignment_in_bytes == 0, "offset must be aligned for the resource");
    CC_ASSERT(offset + reqs.size_in_bytes <= _size_in_bytes, "allocation must lie within the heap");
    return {.heap = shared_from_this(),
            .offset = offset,
            .size_in_bytes = reqs.size_in_bytes,
            .scope = lifetime_scope::persistent};
}

allocation_info memory_heap::acquire_allocation_for_buffer(isize size_in_bytes, buffer_usages usage, isize offset) const
{
    memory_requirements const reqs = memory_requirements_for_buffer(size_in_bytes, usage);
    CC_ASSERT(reqs.alignment_in_bytes > 0, "backend must report a positive alignment");
    CC_ASSERT(offset >= 0 && offset % reqs.alignment_in_bytes == 0, "offset must be aligned for the resource");
    CC_ASSERT(offset + reqs.size_in_bytes <= _size_in_bytes, "allocation must lie within the heap");

    allocation_info info;
    info.heap = shared_from_this();
    info.offset = offset;
    info.size_in_bytes = reqs.size_in_bytes; // backend-reported size (may exceed the requested size)
    info.scope = lifetime_scope::persistent;
    return info;
}
} // namespace sg
