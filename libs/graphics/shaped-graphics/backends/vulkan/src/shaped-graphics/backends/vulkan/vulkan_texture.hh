#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_texture.hh>

/// Vulkan implementation of sg::raw_texture.
/// Holds the VkImage and its backing device-local VkDeviceMemory, always a dedicated allocation.
/// There is no layout tracking yet, so a texture is creatable but unusable in a command list until layout transitions land.
class sg::backend::vulkan::vulkan_texture final : public sg::raw_texture
{
public:
    vulkan_texture(vulkan_context& ctx,
                   sg::epoch created_in,
                   sg::texture_description const& desc,
                   VkImage image,
                   VkDeviceMemory memory)
      : sg::raw_texture(desc), _ctx(ctx), _creation_epoch(created_in), _image(image), _memory(memory)
    {
    }

    // Deferred deletion: hands the GPU handles and finalizers to the context, freed once the owning epoch retires.
    // Freeing them here could pull memory out from under a GPU still reading them.
    // Body in vulkan_texture.cc.
    ~vulkan_texture() override;

    vulkan_context& _ctx;      // creating context — outlives this texture
    sg::epoch _creation_epoch; // epoch this texture was created in (immutable identity / diagnostics)
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
};
