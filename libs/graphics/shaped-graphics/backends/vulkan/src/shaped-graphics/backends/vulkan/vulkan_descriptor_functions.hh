#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>

/// The VK_EXT_descriptor_buffer entry points, loaded once per device.
///
/// An extension's commands are not exported by the loader's static symbols, so they have to be fetched through
/// vkGetDeviceProcAddr rather than called directly.
/// Loaded once and kept on the context, because the alternative is a proc-address lookup per descriptor write.

struct sg::backend::vulkan::vulkan_descriptor_functions
{
    PFN_vkGetDescriptorSetLayoutSizeEXT get_layout_size = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT get_binding_offset = nullptr;
    PFN_vkGetDescriptorEXT get_descriptor = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT cmd_bind_descriptor_buffers = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT cmd_set_descriptor_offsets = nullptr;

    /// Fetches every entry point; returns false when any is missing, which means the extension is not really there.
    [[nodiscard]] bool load(VkDevice device);

    [[nodiscard]] bool is_loaded() const { return get_descriptor != nullptr; }
};
