#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh> // sg::view_class

/// A bound buffer paired with the access class it is used as — the backend-typed input to the dispatch
/// hazard declares (see dx12_command_list::compute_dispatch).
struct sg::backend::dx12::dx12_hazard_view
{
    dx12_buffer_handle buffer;
    sg::view_class access;
};

/// A bound texture paired with the subresource range + access class it is used as — the texture analogue
/// of dx12_hazard_view, declared for layout-transition barriers at dispatch.
struct sg::backend::dx12::dx12_texture_hazard_view
{
    dx12_texture_handle texture;
    sg::subresource_range range;
    sg::view_class access;
};

/// One element of an array binding: the bound resource (exactly one of buffer / texture set, both null when vacant)
/// plus, for a texture, the subresource range its view exposes.
/// Holding the handle keeps the element's resource alive while the group lives — array elements are not pushed
/// into the group's `referenced` vectors.
struct sg::backend::dx12::dx12_array_element
{
    dx12_buffer_handle buffer;
    dx12_texture_handle texture;
    sg::subresource_range range;

    /// Whether this element was bound as a null view — a null descriptor with no resource behind it.
    /// Declaring access on a vacant element is an error (there is nothing to track).
    [[nodiscard]] bool is_vacant() const { return buffer == nullptr && texture == nullptr; }
};

/// An array binding's per-element resources, keyed by the binding's reflection name.
/// The resolution target of `declare_array_*_access`: a declared element index reads this vector.
struct sg::backend::dx12::dx12_array_binding
{
    cc::string name;
    bool is_texture = false;
    cc::vector<dx12_array_element> elements;
};

/// One view to bind, already resolved to its position in the layout's `view_slots`.
///
/// Both inputs reduce to this before anything is written: a `named_view` by looking the name up, a
/// `slotted_view` by the layout's `bindings()`-to-split-table remap.
/// `name` is carried for the error messages, which a slot-keyed caller would otherwise have lost.
struct sg::backend::dx12::dx12_resolved_view
{
    isize slot_index = 0;
    cc::string_view name;
    sg::bound_view const* view = nullptr;
};

/// dx12 binding_group: a contiguous range of descriptors in the context's shader-visible heap, one per layout binding, created from the bound views.
/// `table_start` is the GPU handle the command list binds as a root descriptor table.
///
/// The descriptor range comes from the heap region matching `scope`.
/// A persistent group's table is allocated from the free list and returned to it, epoch-deferred, when the group is released.
/// A transient group's is ring-allocated and reclaimed collectively when its epoch retires.
class sg::backend::dx12::dx12_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create(dx12_context& ctx,
                                                                      dx12_binding_group_layout_handle const& layout,
                                                                      cc::span<sg::named_view const> views,
                                                                      cc::span<sg::named_sampler const> samplers,
                                                                      sg::lifetime_scope scope);

    /// The same, keyed by layout slot rather than by binding name.
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create(dx12_context& ctx,
                                                                      dx12_binding_group_layout_handle const& layout,
                                                                      cc::span<sg::slotted_view const> views,
                                                                      cc::span<sg::named_sampler const> samplers,
                                                                      sg::lifetime_scope scope);

    /// What both overloads run once their input is resolved to slots.
    [[nodiscard]] static cc::result<dx12_binding_group_handle> create_resolved(dx12_context& ctx,
                                                                               dx12_binding_group_layout_handle const& layout,
                                                                               cc::span<dx12_resolved_view const> views,
                                                                               cc::span<sg::named_sampler const> samplers,
                                                                               sg::lifetime_scope scope);

    dx12_binding_group() = default;

    // Returns a persistent group's descriptor range to the free list, deferred until its last-using
    // epoch retires (a transient group's range is reclaimed by the ring, so nothing to free). Body in .cc.
    ~dx12_binding_group() override;

    dx12_context* _ctx = nullptr; // creating context — outlives this group (for the deferred free)
    dx12_binding_group_layout_handle layout;
    D3D12_GPU_DESCRIPTOR_HANDLE table_start = {};
    dx12_descriptor_alloc table; // the group's CBV/SRV/UAV range (its start feeds table_start; count for freeing)
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table_start = {};
    dx12_descriptor_alloc sampler_table; // the group's SAMPLER range (empty if the layout has no dynamic samplers)
    cc::vector<dx12_buffer_handle> referenced;           // keeps the bound buffers alive while the group lives
    cc::vector<dx12_texture_handle> referenced_textures; // keeps the bound textures alive while the group lives
    cc::vector<dx12_hazard_view> hazard_views;           // (buffer + access class) — declared for hazards at dispatch
    cc::vector<dx12_texture_hazard_view> texture_hazard_views; // (texture + range + access) — declared at dispatch

    // Array bindings (count > 1) are not auto-tracked: their elements appear here instead of in the hazard vectors,
    // and the dispatching caller must declare the used elements via declare_array_*_access (resolved against this).
    cc::vector<dx12_array_binding> array_bindings;

    // Transient groups expire when their epoch passes: the ring recycles their descriptor slots, so
    // binding one afterwards is a hard error (checked at bind). Both are inert for a persistent group.
    sg::epoch creation_epoch = sg::epoch::invalid;
    bool transient = false;
};
