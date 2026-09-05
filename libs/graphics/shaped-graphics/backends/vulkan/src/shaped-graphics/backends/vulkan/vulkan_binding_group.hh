#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_descriptor_heap.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh> // sg::view_class

/// A bound buffer paired with the access class it is used as — the backend-typed input to the dispatch hazard declares.
struct sg::backend::vulkan::vulkan_hazard_view
{
    vulkan_buffer_handle buffer;
    sg::view_class access;
};

/// A bound texture paired with the subresource range + access class it is used as.
/// The texture analogue of vulkan_hazard_view, declared for layout transitions at dispatch.
struct sg::backend::vulkan::vulkan_texture_hazard_view
{
    vulkan_texture_handle texture;
    sg::subresource_range range;
    sg::view_class access;
};

/// One element of an array binding: the bound resource (exactly one of buffer / texture set, both null when vacant)
/// plus, for a texture, the subresource range its view exposes.
/// Holding the handle keeps the element's resource alive while the group lives.
struct sg::backend::vulkan::vulkan_array_element
{
    vulkan_buffer_handle buffer;
    vulkan_texture_handle texture;
    sg::subresource_range range;

    /// Whether this element was bound as a vacant view — a zeroed descriptor with no resource behind it.
    /// Declaring access on a vacant element is an error, since there is nothing to track.
    [[nodiscard]] bool is_vacant() const { return buffer == nullptr && texture == nullptr; }
};

/// An array binding's per-element resources, keyed by the binding's reflection name.
/// The resolution target of `declare_array_*_access`.
struct sg::backend::vulkan::vulkan_array_binding
{
    cc::string name;
    bool is_texture = false;
    cc::vector<vulkan_array_element> elements;
};

/// vulkan binding_group: a range of the context's descriptor heap holding one set's descriptors, bound by offset.
///
/// The descriptor-buffer model makes this a *byte range* rather than an object — there is no VkDescriptorSet and no
/// pool behind it, and binding the group is naming an offset into the already-bound heap.
/// That is what makes a staging group's snapshot a memcpy: a minted group's bytes are the staging image's bytes.
///
/// A persistent group's range comes from the heap's free list and goes back to it, epoch-deferred, on release.
/// A transient group's is ring-allocated and reclaimed collectively when its epoch retires.
/// One view to bind, already resolved to its position in `bindings()`.
///
/// Both inputs reduce to this before anything is written: a `named_view` by looking the name up, a
/// `slotted_view` by taking the slot as what it is — a position in `bindings()`, which is what vulkan's
/// undivided descriptor set indexes anyway.
/// `name` is carried for the error messages, which a slot-keyed caller would otherwise have lost.
struct sg::backend::vulkan::vulkan_resolved_view
{
    isize slot = 0;
    cc::string_view name;
    sg::bound_view const* view = nullptr;
};

class sg::backend::vulkan::vulkan_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<vulkan_binding_group_handle> create(vulkan_context& ctx,
                                                                        vulkan_binding_group_layout_handle const& layout,
                                                                        cc::span<sg::named_view const> views,
                                                                        cc::span<sg::named_sampler const> samplers,
                                                                        sg::lifetime_scope scope);

    /// The same, keyed by layout slot rather than by binding name.
    [[nodiscard]] static cc::result<vulkan_binding_group_handle> create(vulkan_context& ctx,
                                                                        vulkan_binding_group_layout_handle const& layout,
                                                                        cc::span<sg::slotted_view const> views,
                                                                        cc::span<sg::named_sampler const> samplers,
                                                                        sg::lifetime_scope scope);

    /// What both overloads run once their input is resolved to slots.
    [[nodiscard]] static cc::result<vulkan_binding_group_handle> create_resolved(
        vulkan_context& ctx,
        vulkan_binding_group_layout_handle const& layout,
        cc::span<vulkan_resolved_view const> views,
        cc::span<sg::named_sampler const> samplers,
        sg::lifetime_scope scope);

    /// Mints a persistent group whose descriptors are a copy of `image`, which must be one layout-sized descriptor
    /// image — what a staging group keeps and hands over unchanged.
    /// The resource bookkeeping is the caller's, since only it knows what the image references.
    [[nodiscard]] static cc::result<vulkan_binding_group_handle> create_from_image(
        vulkan_context& ctx,
        vulkan_binding_group_layout_handle const& layout,
        cc::span<byte const> image,
        cc::vector<vulkan_buffer_handle> referenced,
        cc::vector<vulkan_texture_handle> referenced_textures,
        cc::vector<vulkan_hazard_view> hazard_views,
        cc::vector<vulkan_texture_hazard_view> texture_hazard_views,
        cc::vector<vulkan_array_binding> array_bindings);

    vulkan_binding_group() = default;

    // Returns a persistent group's range to the heap's free list, deferred until its last-using epoch retires
    // (a transient group's range is reclaimed by the ring, so nothing to free). Body in .cc.
    ~vulkan_binding_group() override;

    vulkan_context* _ctx = nullptr; // creating context — outlives this group (for the deferred free)
    vulkan_binding_group_layout_handle layout;
    vulkan_descriptor_range range;                         // this group's descriptors within the heap
    cc::vector<vulkan_buffer_handle> referenced;           // keeps the bound buffers alive while the group lives
    cc::vector<vulkan_texture_handle> referenced_textures; // keeps the bound textures alive while the group lives
    cc::vector<vulkan_hazard_view> hazard_views;           // (buffer + access class) — declared for hazards at dispatch
    cc::vector<vulkan_texture_hazard_view> texture_hazard_views; // (texture + range + access) — declared at dispatch

    // Array bindings (count > 1) are not auto-tracked: their elements appear here instead of in the hazard vectors,
    // and the dispatching caller must declare the used elements via declare_array_*_access (resolved against this).
    cc::vector<vulkan_array_binding> array_bindings;

    // Transient groups expire when their epoch passes: the ring recycles their descriptor bytes, so binding one
    // afterwards is a hard error (checked at bind). Both are inert for a persistent group.
    sg::epoch creation_epoch = sg::epoch::invalid;
    bool transient = false;
};
