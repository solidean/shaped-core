#include "vulkan-test-common.hh"

#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

using namespace cc::primitive_defines;

// An image someone else created — an XR runtime's swapchain image — as an sg texture.
// The validation layer is what checks the starting layout: a tracker that disagrees with the image's real layout makes
// the first barrier name the wrong old layout, and that message fails the test through the log rule.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_size = 16;

sg::texture_description target_desc()
{
    auto d = sg::texture_description();
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = k_size;
    d.height = k_size;
    d.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    return d;
}

/// An image and its memory created outside sg, the way a runtime creates one.
struct foreign_image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

foreign_image make_foreign_target(vulkan::vulkan_context& ctx)
{
    auto const info = VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = u32(k_size), .height = u32(k_size), .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto out = foreign_image();
    REQUIRE(vkCreateImage(ctx.device(), &info, nullptr, &out.image) == VK_SUCCESS);

    auto req = VkMemoryRequirements{};
    vkGetImageMemoryRequirements(ctx.device(), out.image, &req);
    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = ctx.find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    REQUIRE(vkAllocateMemory(ctx.device(), &alloc, nullptr, &out.memory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(ctx.device(), out.image, out.memory, 0) == VK_SUCCESS);
    return out;
}

/// Clears `tex` to `color`, hands it back at `render_target`, and returns whether every texel read back as `rgba`.
cc::shared_async<bool> clear_and_read(vulkan::vulkan_context_handle ctx,
                                      sg::raw_texture_handle tex,
                                      tg::vec4f color,
                                      u32 rgba)
{
    auto const typed = sg::texture_2d::from_raw(tex);
    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(color)}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    // What a runtime's release asks for: the image back in the layout it handed it out in.
    cmd->ensure_layout(tex, sg::texture_layout::render_target);
    ctx->submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    if (bytes.size() != isize(k_size) * k_size * 4)
        co_return false;
    auto const* px = reinterpret_cast<u32 const*>(bytes.data());
    for (auto i = 0; i < k_size * k_size; ++i)
        if (px[i] != rgba)
            co_return false;
    co_return true;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg vulkan - a wrapped foreign image starts where its owner left it, and goes back there",
                     (vulkan::vulkan_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const foreign = make_foreign_target(*ctx);

    // First as a fresh image, which sg moves out of UNDEFINED itself.
    auto fresh = ctx->wrap_vulkan_texture(foreign.image, target_desc(), sg::texture_layout::undefined);
    REQUIRE(fresh.has_value());
    CHECK(co_await clear_and_read(ctx, fresh.value(), tg::vec4f(1, 0, 0, 1), 0xFF0000FFu));

    // Then as a runtime hands one out, already at render_target, which sg must not transition from UNDEFINED again.
    auto handed_out = ctx->wrap_vulkan_texture(foreign.image, target_desc(), sg::texture_layout::render_target);
    REQUIRE(handed_out.has_value());
    CHECK(co_await clear_and_read(ctx, handed_out.value(), tg::vec4f(0, 1, 0, 1), 0xFF00FF00u));
    CHECK(ctx->current_texture_layout(handed_out.value(), sg::subresource_range{}) == sg::texture_layout::render_target);

    // The image is ours, so it goes once no list can still be touching it.
    auto const device = ctx->device();
    ctx->defer_until_retired(
        [device, foreign]
        {
            vkDestroyImage(device, foreign.image, nullptr);
            vkFreeMemory(device, foreign.memory, nullptr);
        });
}
