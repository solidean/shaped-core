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

/// The identity of one cached VkImageView: the viewed texture plus every field that reaches vkCreateImageView.
///
/// A key rather than a bare hash, so a collision cannot hand back another view's VkImageView — and, since a view is
/// forgotten by key when its texture dies, cannot let one texture's finalizer destroy another's live entry.
/// `hash` and `operator==` fold exactly the same fields, which is the invariant a hash map keyed on this needs.
///
/// **The texture is named by `vulkan_texture::_identity`, never by its address.**
/// A handle would keep every viewed texture alive for the context's life, which is what the eviction below exists to
/// avoid — but an address is not an identity either, since it is reusable the moment the texture object dies and the
/// entry naming it outlives that by up to the in-flight epoch depth.
struct sg::backend::vulkan::vulkan_image_view_key
{
    u64 texture_identity = 0;
    sg::view_class access = sg::view_class::readonly;
    sg::texture_view_dimension dimension = sg::texture_view_dimension::tex_2d;
    sg::pixel_format format = sg::pixel_format::undefined;
    sg::subresource_range range;
    cc::start_end depth_slice_range = {.start = 0, .end = 0};

    [[nodiscard]] friend bool operator==(vulkan_image_view_key const&, vulkan_image_view_key const&) = default;

    /// Folds exactly what `operator==` compares; defaulting the operator is what keeps the two agreeing as fields
    /// are added.
    [[nodiscard]] friend u64 hash(vulkan_image_view_key const& k)
    {
        return cc::make_hash(k.texture_identity, k.access, k.dimension, k.format, k.range, k.depth_slice_range.start,
                             k.depth_slice_range.end);
    }
};

/// Creates and owns the VkImageViews a texture descriptor needs.
/// One per context; a view is created on first use and lives until its texture is released.
///
/// **A cached view is dropped with its texture, and that is load-bearing rather than tidy.**
/// The drop rides the texture's own finalizer, so it happens exactly when the VkImage does: at epoch retire, once
/// the GPU is done with both.
///
/// That deferral is why both keys name the texture by `vulkan_texture::_identity` and not by address.
/// A transient texture is destroyed and recreated every frame, and the allocator hands the new one the dead one's
/// address long before the entry naming it is evicted — so an address key lets the new texture inherit the old
/// entry and render through a view of an image that is about to be freed under the GPU.
class sg::backend::vulkan::vulkan_image_view_cache
{
public:
    explicit vulkan_image_view_cache(vulkan_context& ctx) : _ctx(ctx) {}
    ~vulkan_image_view_cache();

    /// The image view for `view`, created on first request.
    /// Keyed on the view's identity — the texture's identity stamp plus every field that reaches a descriptor — so
    /// this never conflates two views that would produce different descriptors.
    [[nodiscard]] VkImageView acquire(sg::raw_texture_view const& view);

    /// The image view for a rendering-scope attachment, created on first request.
    ///
    /// A render-target or depth-stencil view never enters a descriptor, so it is keyed in its own map rather than
    /// sharing the shader views' one — and it is cached at all, where dx12 allocates an RTV/DSV slot per scope and
    /// frees it epoch-deferred at end_rendering.
    /// Caching removes that entire dance: a scope's attachments are the same few views frame after frame.
    [[nodiscard]] VkImageView acquire_attachment(sg::raw_texture_handle const& texture,
                                                 sg::texture_view_dimension dimension,
                                                 sg::pixel_format format,
                                                 sg::subresource_range const& range);

    void shutdown();

private:
    using view_map = cc::map<vulkan_image_view_key, VkImageView>;

    /// Registers a finalizer on `texture` that erases `key` from `map` and destroys the view it named.
    /// Called once per entry, when the entry is created.
    void forget_with_texture(sg::raw_texture const& texture, cc::mutex<view_map>& map, vulkan_image_view_key key);

    vulkan_context& _ctx;
    cc::mutex<view_map> _views;
    cc::mutex<view_map> _attachment_views;
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

/// Writes `sampler`'s descriptor into `dst`.
/// A sampler descriptor always names an object, so `sampler` must be non-null — there is no vacant sampler, since an
/// unsupplied dynamic sampler is the default sampler state rather than an absence.
void write_sampler_descriptor(vulkan_context& ctx, VkSampler sampler, byte* dst);

/// The device's descriptor size for a binding's type, which is what a set layout reserves per element.
[[nodiscard]] isize descriptor_size_of(vulkan_context const& ctx, sg::binding_type type);
} // namespace sg::backend::vulkan
