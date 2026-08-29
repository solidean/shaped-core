#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// Turning an sg `raw_view` into the bytes a descriptor is.
///
/// Under VK_EXT_descriptor_buffer a descriptor is not an opaque handle written by an API call into an allocated set —
/// it is a small blob `vkGetDescriptorEXT` produces, which the caller places wherever it likes.
/// So this writes into memory the descriptor heap owns, at an offset the set layout dictates.
///
/// Buffers are named by device address rather than by VkBuffer handle, which is why every buffer carries one.
/// Textures need a VkImageView, which is created here and cached by the view's own identity hash — sg already defines
/// that hash over exactly the fields reaching a descriptor, so two equal views share one image view by construction.

/// Creates and owns the VkImageViews a texture descriptor needs.
/// One per context; a view is created on first use and lives until the context goes.
class sg::backend::vulkan::vulkan_image_view_cache
{
public:
    explicit vulkan_image_view_cache(vulkan_context& ctx) : _ctx(ctx) {}
    ~vulkan_image_view_cache();

    /// The image view for `view`, created on first request.
    /// Keyed on the view's identity hash, which sg defines over the resource address plus every field that reaches a
    /// descriptor — so this never conflates two views that would produce different descriptors.
    [[nodiscard]] VkImageView acquire(sg::raw_texture_view const& view);

    void shutdown();

private:
    vulkan_context& _ctx;
    cc::mutex<cc::map<u64, VkImageView>> _views;
};

namespace sg::backend::vulkan
{
/// Writes the descriptor for `view` into `dst`, which must have room for the device's size for that descriptor type.
///
/// `binding` says what kind of descriptor is expected, which is what lets a `vacant_view` be written at all: an empty
/// element carries no resource, so its descriptor is synthesized from the binding's declared type and dimension.
void write_view_descriptor(vulkan_context& ctx,
                           vulkan_image_view_cache& image_views,
                           sg::binding const& binding,
                           sg::raw_view const& view,
                           byte* dst);

/// The device's descriptor size for a binding's type, which is what a set layout reserves per element.
[[nodiscard]] isize descriptor_size_of(vulkan_context const& ctx, sg::binding_type type);
} // namespace sg::backend::vulkan
