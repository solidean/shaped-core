#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/binding/binding_group.hh>

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
};
