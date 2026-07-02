#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/buffer.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::buffer. Holds the VkBuffer and its backing device-local
/// VkDeviceMemory (sg exposes no host-visible buffers). Both are VK_NULL_HANDLE for an empty
/// (size 0) buffer.
class vulkan_buffer final : public sg::buffer
{
public:
    vulkan_buffer(vulkan_context& ctx,
                  sg::epoch created_in,
                  cc::isize size_in_bytes,
                  sg::buffer_usage usage,
                  VkBuffer buffer,
                  VkDeviceMemory memory)
      : sg::buffer(size_in_bytes, usage), _ctx(ctx), _creation_epoch(created_in), _buffer(buffer), _memory(memory)
    {
    }

    // Deferred deletion: hands the GPU handles + finalizers to the context, freed once the owning
    // epoch retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~vulkan_buffer() override;

    vulkan_context& _ctx;      // creating context — outlives this buffer
    sg::epoch _creation_epoch; // epoch this buffer was created in (immutable identity / diagnostics)
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
};
} // namespace sg::backend::vulkan
