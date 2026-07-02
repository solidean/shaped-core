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
    vulkan_buffer(vulkan_context& ctx, cc::isize size_in_bytes, sg::buffer_usage usage, VkBuffer buffer, VkDeviceMemory memory)
      : sg::buffer(size_in_bytes, usage), _ctx(ctx), _buffer(buffer), _memory(memory)
    {
    }

    ~vulkan_buffer() override; // destroys _buffer + frees _memory; body in vulkan_buffer.cc

    vulkan_context& _ctx; // creating context — outlives this buffer
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
};
} // namespace sg::backend::vulkan
