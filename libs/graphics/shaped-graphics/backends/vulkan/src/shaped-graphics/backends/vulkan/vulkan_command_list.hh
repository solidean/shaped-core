#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::command_list. Owns its command pool and the single command buffer
/// allocated from it, handed out already recording. Recording methods (copy/upload/download buffer,
/// ...) land here with the first milestone.
class vulkan_command_list final : public sg::command_list
{
public:
    vulkan_command_list(vulkan_context& ctx, VkCommandPool pool, VkCommandBuffer buffer)
      : _ctx(ctx), _pool(pool), _buffer(buffer)
    {
    }

    ~vulkan_command_list() override; // destroys _pool (and its buffer); body in vulkan_command_list.cc

    vulkan_context& _ctx; // creating context — outlives this list
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _buffer = VK_NULL_HANDLE; // owned by _pool, freed with it
};
} // namespace sg::backend::vulkan
