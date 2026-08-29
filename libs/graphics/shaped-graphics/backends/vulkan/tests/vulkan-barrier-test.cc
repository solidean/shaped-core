#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>

// The sg access vocabulary translated into synchronization2 masks.
// Pure logic with no Vulkan device in it, so unlike the rest of this suite these run everywhere — which is the whole
// reason the translation lives in its own file rather than inside the command list.

namespace
{
namespace vulkan = sg::backend::vulkan;
}

TEST("sg vulkan - an empty stage or access set maps to NONE")
{
    // What a layout-only transition carries: no stage, no access, just a change of layout.
    // Vulkan requires the two to agree, and NONE/NONE is the pairing it accepts.
    CHECK(vulkan::vk_stage2_from({}) == VK_PIPELINE_STAGE_2_NONE);
    CHECK(vulkan::vk_access2_from({}) == VK_ACCESS_2_NONE);
}

TEST("sg vulkan - vertex stage covers input assembly as well as the shaders")
{
    // sg folds all vertex processing into one stage, so index and vertex fetch must have a legal stage to pair with.
    // D3D12 needs a special case here (it rejects VERTEX_SHADING with INDEX_BUFFER); Vulkan spells the input stages
    // separately, so including them is what keeps an index-buffer barrier valid.
    auto const stages = vulkan::vk_stage2_from(sg::pipeline_stage_flag::vertex);
    CHECK((stages & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT) != 0);
    CHECK((stages & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) != 0);
}

TEST("sg vulkan - a stage set maps to the union of its members")
{
    auto const stages = vulkan::vk_stage2_from(sg::pipeline_stage_flag::compute | sg::pipeline_stage_flag::copy);
    CHECK(stages == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT));
}

TEST("sg vulkan - depth target maps to both fragment-test stages")
{
    auto const stages = vulkan::vk_stage2_from(sg::pipeline_stage_flag::depth_stencil_target);
    CHECK(stages == (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
}

TEST("sg vulkan - access flags map one to one")
{
    CHECK(vulkan::vk_access2_from(sg::access_flag::copy_write) == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(vulkan::vk_access2_from(sg::access_flag::shader_read) == VK_ACCESS_2_SHADER_READ_BIT);
    CHECK(vulkan::vk_access2_from(sg::access_flag::index_read) == VK_ACCESS_2_INDEX_READ_BIT);
    CHECK(vulkan::vk_access2_from(sg::access_flag::copy_read | sg::access_flag::copy_write)
          == (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT));
}

TEST("sg vulkan - both storage layouts collapse onto GENERAL")
{
    // Vulkan has no separate storage-image layout, so shader_readwrite and general are the same image layout.
    // They stay distinct in sg because D3D12 does separate them.
    CHECK(vulkan::vk_layout_from(sg::texture_layout::shader_readwrite) == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(vulkan::vk_layout_from(sg::texture_layout::general) == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(vulkan::vk_layout_from(sg::texture_layout::shader_readonly) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vulkan::vk_layout_from(sg::texture_layout::copy_dst) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(vulkan::vk_layout_from(sg::texture_layout::present) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST("sg vulkan - an undefined source layout is the discard")
{
    // sg says "previous contents are not preserved" by declaring the source layout undefined, which is exactly what
    // Vulkan's UNDEFINED old layout already means — so nothing extra has to be flagged.
    CHECK(vulkan::vk_layout_from(sg::texture_layout::undefined) == VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST("sg vulkan - aspect masks come from the range's aspect span")
{
    auto color = sg::subresource_range();
    CHECK(vulkan::vk_aspect_mask_from(color) == VK_IMAGE_ASPECT_COLOR_BIT);

    // A depth+stencil range spans two aspects and must produce both bits in one mask.
    auto depth_stencil = sg::subresource_range();
    depth_stencil.aspect_range = {.start = int(sg::texture_aspect::depth), .end = int(sg::texture_aspect::stencil) + 1};
    CHECK(vulkan::vk_aspect_mask_from(depth_stencil) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
}

TEST("sg vulkan - an image barrier carries the range as counts, not endpoints")
{
    // sg ranges are half-open [start, end); Vulkan takes a base plus a count, so the conversion is where an
    // off-by-one would hide.
    auto range = sg::subresource_range();
    range.mip_range = {.start = 2, .end = 5};
    range.array_range = {.start = 1, .end = 7};

    auto barrier = sg::access_barrier();
    barrier.needed = true;
    barrier.src_stages = sg::pipeline_stage_flag::copy;
    barrier.dst_stages = sg::pipeline_stage_flag::fragment;
    barrier.src_access = sg::access_flag::copy_write;
    barrier.dst_access = sg::access_flag::shader_read;
    barrier.src_layout = sg::texture_layout::copy_dst;
    barrier.dst_layout = sg::texture_layout::shader_readonly;

    auto const b = vulkan::make_image_barrier(VkImage(nullptr), range, barrier);
    CHECK(b.subresourceRange.baseMipLevel == 2);
    CHECK(b.subresourceRange.levelCount == 3);
    CHECK(b.subresourceRange.baseArrayLayer == 1);
    CHECK(b.subresourceRange.layerCount == 6);
    CHECK(b.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(b.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(b.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(b.dstAccessMask == VK_ACCESS_2_SHADER_READ_BIT);
}

TEST("sg vulkan - a buffer barrier covers the whole buffer")
{
    // sg tracks buffer access per resource rather than per byte range, so there is nothing narrower to scope to.
    auto barrier = sg::access_barrier();
    barrier.needed = true;
    barrier.src_access = sg::access_flag::shader_write;
    barrier.dst_access = sg::access_flag::copy_read;

    auto const b = vulkan::make_buffer_barrier(VkBuffer(nullptr), barrier);
    CHECK(b.offset == 0);
    CHECK(b.size == VK_WHOLE_SIZE);
    CHECK(b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
}
