#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/subresource.hh>

/// vulkan owns its barrier emission entirely — the sg core only hands it the `access_barrier` computed by the
/// shared `resource_access_state` machine, and this file turns one of those into Vulkan's synchronization2 structs.
///
/// The sg vocabulary was written against `PIPELINE_STAGE_2` / `ACCESS_2` in the first place, so every mapping below
/// is one-to-one and the interesting work is elsewhere: what to declare, and when to flush.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace sg::backend::vulkan
{
/// The stage mask an sg stage set implies; an empty set is `NONE`, which is what a layout-only transition wants.
[[nodiscard]] VkPipelineStageFlags2 vk_stage2_from(sg::pipeline_stage_flags stages);

/// The access mask an sg access set implies; an empty set is `NONE`.
[[nodiscard]] VkAccessFlags2 vk_access2_from(sg::access_flags access);

/// The image layout an sg texture layout means.
/// `shader_image` and `general` both map to `VK_IMAGE_LAYOUT_GENERAL` — Vulkan has no separate storage layout.
[[nodiscard]] VkImageLayout vk_layout_from(sg::texture_layout layout);

/// The aspect mask a subresource range's aspect span covers.
[[nodiscard]] VkImageAspectFlags vk_aspect_mask_from(sg::subresource_range const& range, sg::pixel_format format);

/// A whole-buffer memory barrier for `barrier`.
/// Vulkan can scope a buffer barrier to a byte range, but sg tracks access per resource rather than per range, so
/// there is nothing narrower to say and the barrier covers the whole buffer.
[[nodiscard]] VkBufferMemoryBarrier2 make_buffer_barrier(VkBuffer buffer, sg::access_barrier const& barrier);

/// An image barrier for `barrier`, scoped to `range`.
/// A `src_layout` of `undefined` means the previous contents are not preserved, which is exactly what Vulkan's
/// `VK_IMAGE_LAYOUT_UNDEFINED` as an old layout already says — so a discard needs no separate flag.
/// `format` is the texture's own, and is what turns the range's positional aspect indices into aspect bits.
[[nodiscard]] VkImageMemoryBarrier2 make_image_barrier(VkImage image,
                                                       sg::subresource_range const& range,
                                                       sg::pixel_format format,
                                                       sg::access_barrier const& barrier);

/// Records one `vkCmdPipelineBarrier2` for everything staged, or nothing at all when every span is empty.
void submit_barriers(VkCommandBuffer cmd,
                     cc::span<VkBufferMemoryBarrier2 const> buffer_barriers,
                     cc::span<VkImageMemoryBarrier2 const> image_barriers,
                     cc::span<VkMemoryBarrier2 const> memory_barriers = {});

/// Folds a buffer barrier into `global`, which then orders everything `b` did and more.
void merge_into_memory_barrier(VkMemoryBarrier2& global, VkBufferMemoryBarrier2 const& b);

/// A memory barrier that orders nothing yet, for merge_into_memory_barrier to widen.
[[nodiscard]] VkMemoryBarrier2 make_empty_memory_barrier();

/// Adds the batch `submit_barriers` would record to `sink`'s stats: its records by kind, and one call if it is not empty.
/// `sink` is a list's sg::impl::stat_counts or the context's sg::impl::stat_totals.
template <class Sink>
void count_barriers(Sink& sink,
                    cc::span<VkBufferMemoryBarrier2 const> buffer_barriers,
                    cc::span<VkImageMemoryBarrier2 const> image_barriers,
                    cc::span<VkMemoryBarrier2 const> memory_barriers = {})
{
    if (buffer_barriers.empty() && image_barriers.empty() && memory_barriers.empty())
        return;
    sink.add(sg::stat::barrier_calls);
    sink.add(sg::stat::buffer_barriers, buffer_barriers.size());
    sink.add(sg::stat::texture_barriers, image_barriers.size());
    sink.add(sg::stat::global_barriers, memory_barriers.size());
    auto transitions = isize(0);
    for (auto const& b : image_barriers)
        if (b.oldLayout != b.newLayout)
            ++transitions;
    sink.add(sg::stat::texture_transitions, transitions);
}
} // namespace sg::backend::vulkan
