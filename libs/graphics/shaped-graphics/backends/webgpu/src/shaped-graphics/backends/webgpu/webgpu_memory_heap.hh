#pragma once

#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/memory/memory_heap.hh>

/// A memory heap that places nothing.
///
/// WebGPU exposes no memory heaps, so a buffer "placed" here is created with an allocation of its own and the placement is ignored, silently.
/// The heap still answers requirements honestly — a whole number of words, at word alignment — so a caller's own allocator stays exactly as correct as on a backend that places.
/// It is what lets ctx.transient's bump allocator run unchanged on WebGPU.
class sg::backend::webgpu::webgpu_memory_heap final : public sg::memory_heap
{
public:
    explicit webgpu_memory_heap(isize size_in_bytes) : sg::memory_heap(size_in_bytes) {}

protected:
    [[nodiscard]] sg::memory_requirements query_buffer_requirements(isize size_in_bytes,
                                                                    sg::buffer_usages usage) const override;
};
