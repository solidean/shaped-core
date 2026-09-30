#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/resource/views.hh>

/// WebGPU implementation of sg::binding_group: one WGPUBindGroup.
///
/// It holds the resources it binds, since an sg group keeps its views alive and a WebGPU group only keeps the objects.
/// A transient group is not recycled — WebGPU owns the object — so its scope changes nothing but the expiry check at bind.
class sg::backend::webgpu::webgpu_binding_group final : public sg::binding_group
{
public:
    [[nodiscard]] static cc::result<webgpu_binding_group_handle> create(webgpu_context& ctx,
                                                                        webgpu_binding_group_layout_handle const& layout,
                                                                        cc::span<sg::named_view const> views,
                                                                        cc::span<sg::named_sampler const> samplers,
                                                                        sg::lifetime_scope scope);

    [[nodiscard]] static cc::result<webgpu_binding_group_handle> create(webgpu_context& ctx,
                                                                        webgpu_binding_group_layout_handle const& layout,
                                                                        cc::span<sg::slotted_view const> views,
                                                                        cc::span<sg::named_sampler const> samplers,
                                                                        sg::lifetime_scope scope);

    [[nodiscard]] WGPUBindGroup raw() const { return _group.get(); }

    webgpu_binding_group_layout_handle layout;
    sg::epoch creation_epoch = sg::epoch::invalid;
    bool transient = false;

    wgpu_bind_group _group;
    cc::vector<sg::raw_buffer_handle> referenced_buffers;
    cc::vector<sg::raw_texture_handle> referenced_textures;

    /// The bound TLASes, whose regions must outlive the group.
    cc::vector<sg::tlas_handle> referenced_tlases;

    /// Each acceleration member's root unit, in the layout's acceleration_slots() order; 0, the empty TLAS, for a null one.
    cc::vector<u32> acceleration_roots;

    /// A bound buffer or texture, by identity, with the binding it sits at and the view it is bound through.
    /// What a draw orders against: WebGPU tracks usage itself, but never between two draws of one pass.
    struct bound_resource
    {
        void const* resource = nullptr;
        isize binding = -1;
        sg::view_class bound_as = sg::view_class::readonly;
    };
    cc::vector<bound_resource> bound;
};
