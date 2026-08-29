#include <shaped-graphics/backends/vulkan/vulkan_descriptor_functions.hh>

namespace sg::backend::vulkan
{
bool vulkan_descriptor_functions::load(VkDevice device)
{
    auto const fetch = [device](char const* name) { return vkGetDeviceProcAddr(device, name); };

    get_layout_size = PFN_vkGetDescriptorSetLayoutSizeEXT(fetch("vkGetDescriptorSetLayoutSizeEXT"));
    get_binding_offset
        = PFN_vkGetDescriptorSetLayoutBindingOffsetEXT(fetch("vkGetDescriptorSetLayoutBindingOffsetEXT"));
    get_descriptor = PFN_vkGetDescriptorEXT(fetch("vkGetDescriptorEXT"));
    cmd_bind_descriptor_buffers = PFN_vkCmdBindDescriptorBuffersEXT(fetch("vkCmdBindDescriptorBuffersEXT"));
    cmd_set_descriptor_offsets = PFN_vkCmdSetDescriptorBufferOffsetsEXT(fetch("vkCmdSetDescriptorBufferOffsetsEXT"));

    // All or nothing: a partially-resolved extension is a driver bug, and treating it as present would fail later at
    // a call site with far less context than here.
    return get_layout_size != nullptr && get_binding_offset != nullptr && get_descriptor != nullptr
        && cmd_bind_descriptor_buffers != nullptr && cmd_set_descriptor_offsets != nullptr;
}
} // namespace sg::backend::vulkan
