#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::raw_texture. Holds the VkImage and its backing device-local
/// VkDeviceMemory (dedicated allocation). Minimal: no per-command-list layout tracking yet — a texture
/// is creatable but not usable in command lists until layout transitions land.
class vulkan_texture final : public sg::raw_texture
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

    // Deferred deletion: hands the GPU handles + finalizers to the context, freed once the owning epoch
    // retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~vulkan_texture() override;

    vulkan_context& _ctx;      // creating context — outlives this texture
    sg::epoch _creation_epoch; // epoch this texture was created in (immutable identity / diagnostics)
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
};
} // namespace sg::backend::vulkan
