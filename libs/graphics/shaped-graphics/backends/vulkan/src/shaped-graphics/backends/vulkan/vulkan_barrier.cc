#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>

namespace sg::backend::vulkan
{
VkPipelineStageFlags2 vk_stage2_from(sg::pipeline_stage_flags stages)
{
    VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
    if (stages.has(sg::pipeline_stage_flag::draw_indirect))
        out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (stages.has(sg::pipeline_stage_flag::vertex))
        // sg's `vertex` covers all vertex processing, input assembly included, so index and vertex fetch have a legal
        // stage to pair with.
        // D3D12 needs a special case here because it rejects VERTEX_SHADING with INDEX_BUFFER.
        // Vulkan spells the input stages separately, so the union is simply correct.
        out |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    if (stages.has(sg::pipeline_stage_flag::fragment))
        out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (stages.has(sg::pipeline_stage_flag::compute))
        out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (stages.has(sg::pipeline_stage_flag::copy))
        out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    if (stages.has(sg::pipeline_stage_flag::render_target))
        out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (stages.has(sg::pipeline_stage_flag::depth_stencil_target))
        out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (stages.has(sg::pipeline_stage_flag::raytracing))
        out |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    if (stages.has(sg::pipeline_stage_flag::accel_build))
        out |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    return out;
}

VkAccessFlags2 vk_access2_from(sg::access_flags access)
{
    VkAccessFlags2 out = VK_ACCESS_2_NONE;
    if (access.has(sg::access_flag::uniform_read))
        out |= VK_ACCESS_2_UNIFORM_READ_BIT;
    if (access.has(sg::access_flag::index_read))
        out |= VK_ACCESS_2_INDEX_READ_BIT;
    if (access.has(sg::access_flag::vertex_read))
        out |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    if (access.has(sg::access_flag::shader_read))
        out |= VK_ACCESS_2_SHADER_READ_BIT;
    if (access.has(sg::access_flag::shader_write))
        out |= VK_ACCESS_2_SHADER_WRITE_BIT;
    if (access.has(sg::access_flag::copy_read))
        out |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (access.has(sg::access_flag::copy_write))
        out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (access.has(sg::access_flag::indirect_read))
        out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (access.has(sg::access_flag::color_write))
        out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (access.has(sg::access_flag::depth_read))
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (access.has(sg::access_flag::depth_write))
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (access.has(sg::access_flag::accel_read))
        out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    if (access.has(sg::access_flag::accel_write))
        out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    return out;
}

VkImageLayout vk_layout_from(sg::texture_layout layout)
{
    switch (layout)
    {
    case sg::texture_layout::undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case sg::texture_layout::general:
        return VK_IMAGE_LAYOUT_GENERAL;
    case sg::texture_layout::shader_readonly:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case sg::texture_layout::shader_readwrite:
        return VK_IMAGE_LAYOUT_GENERAL; // Vulkan has no storage-image layout; GENERAL is the storage-capable one
    case sg::texture_layout::render_target:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case sg::texture_layout::depth_readonly:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case sg::texture_layout::depth_readwrite:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case sg::texture_layout::copy_src:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case sg::texture_layout::copy_dst:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case sg::texture_layout::present:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    CC_UNREACHABLE("unhandled texture_layout in vk_layout_from");
}

VkImageAspectFlags vk_aspect_mask_from(sg::subresource_range const& range, sg::pixel_format format)
{
    VkImageAspectFlags mask = 0;
    for (auto i = range.aspect_range.start; i < range.aspect_range.end; ++i)
        switch (sg::format_aspect_at(format, i))
        {
        case sg::texture_aspect::color:
            mask |= VK_IMAGE_ASPECT_COLOR_BIT;
            break;
        case sg::texture_aspect::depth:
            mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case sg::texture_aspect::stencil:
            mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        case sg::texture_aspect::plane0:
            mask |= VK_IMAGE_ASPECT_PLANE_0_BIT;
            break;
        case sg::texture_aspect::plane1:
            mask |= VK_IMAGE_ASPECT_PLANE_1_BIT;
            break;
        case sg::texture_aspect::plane2:
            mask |= VK_IMAGE_ASPECT_PLANE_2_BIT;
            break;
        }
    CC_ASSERT(mask != 0, "a subresource range covers no aspect");
    return mask;
}

VkBufferMemoryBarrier2 make_buffer_barrier(VkBuffer buffer, sg::access_barrier const& barrier)
{
    return VkBufferMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = vk_stage2_from(barrier.src_stages),
        .srcAccessMask = vk_access2_from(barrier.src_access),
        .dstStageMask = vk_stage2_from(barrier.dst_stages),
        .dstAccessMask = vk_access2_from(barrier.dst_access),
        // Always IGNORED, even with a dedicated transfer queue in the picture.
        // Anything an async transfer can touch is created VK_SHARING_MODE_CONCURRENT over both families, so no
        // ownership transfer is ever needed — and an ownership transfer is precisely what would serialize the
        // concurrency async transfer exists to provide.
        // See vulkan_buffer.cc, which argues the trade.
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
}

VkImageMemoryBarrier2 make_image_barrier(VkImage image,
                                         sg::subresource_range const& range,
                                         sg::pixel_format format,
                                         sg::access_barrier const& barrier)
{
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = vk_stage2_from(barrier.src_stages),
        .srcAccessMask = vk_access2_from(barrier.src_access),
        .dstStageMask = vk_stage2_from(barrier.dst_stages),
        .dstAccessMask = vk_access2_from(barrier.dst_access),
        .oldLayout = vk_layout_from(barrier.src_layout),
        .newLayout = vk_layout_from(barrier.dst_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = vk_aspect_mask_from(range, format),
                .baseMipLevel = u32(range.mip_range.start),
                .levelCount = u32(range.mip_range.end - range.mip_range.start),
                .baseArrayLayer = u32(range.array_range.start),
                .layerCount = u32(range.array_range.end - range.array_range.start),
            },
    };
}

void submit_barriers(VkCommandBuffer cmd,
                     cc::span<VkBufferMemoryBarrier2 const> buffer_barriers,
                     cc::span<VkImageMemoryBarrier2 const> image_barriers)
{
    if (buffer_barriers.empty() && image_barriers.empty())
        return;

    auto const dependency = VkDependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = u32(buffer_barriers.size()),
        .pBufferMemoryBarriers = buffer_barriers.data(),
        .imageMemoryBarrierCount = u32(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}
} // namespace sg::backend::vulkan
