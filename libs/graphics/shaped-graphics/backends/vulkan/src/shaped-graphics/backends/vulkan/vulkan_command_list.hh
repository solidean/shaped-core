#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::command_list. Owns its command pool and the single command buffer
/// allocated from it, handed out already recording. Buffer transfer is not implemented yet.
class vulkan_command_list final : public sg::command_list
{
public:
    vulkan_command_list(vulkan_context& ctx, sg::epoch created_in, VkCommandPool pool, VkCommandBuffer buffer)
      : sg::command_list(created_in), _ctx(ctx), _pool(pool), _buffer(buffer)
    {
    }

    // TODO: inline buffer transfer for the vulkan backend (see the dx12 backend for the reference impl).
    void upload_to_buffer(sg::buffer_handle, cc::span<cc::byte const>, cc::isize) override
    {
        CC_UNREACHABLE("vulkan inline buffer upload is not implemented yet");
    }
    [[nodiscard]] sg::bytes_future download_from_buffer(sg::buffer_handle, cc::isize, cc::isize) override
    {
        CC_UNREACHABLE("vulkan inline buffer download is not implemented yet");
    }

    ~vulkan_command_list() override; // destroys _pool (and its buffer); body in vulkan_command_list.cc

    vulkan_context& _ctx; // creating context — outlives this list
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _buffer = VK_NULL_HANDLE; // owned by _pool, freed with it
};
} // namespace sg::backend::vulkan
