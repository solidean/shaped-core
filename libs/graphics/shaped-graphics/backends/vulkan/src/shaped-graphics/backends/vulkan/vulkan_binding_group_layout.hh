#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/fwd.hh>

/// One group's schema, as a VkDescriptorSetLayout.
///
/// Simpler than the dx12 realization in one respect: D3D12 forbids mixing sampler and resource descriptors in one
/// table, so dx12 splits every layout into two and carries a second heap for samplers.
/// Vulkan puts them in the same set layout, so there is one object here and no split to keep in sync.
///
/// A binding named in the layout's static samplers becomes an *immutable* sampler on the set layout, baked in at
/// creation and not writable afterwards — which is exactly what "static" means at the sg level.
class sg::backend::vulkan::vulkan_binding_group_layout final : public sg::binding_group_layout
{
public:
    [[nodiscard]] static cc::result<vulkan_binding_group_layout_handle>
    create(vulkan_context& ctx, cc::span<sg::binding const> bindings, cc::span<sg::named_sampler const> static_samplers);

    vulkan_binding_group_layout(vulkan_context& ctx,
                                cc::hash128 structural_hash,
                                cc::vector<sg::binding> bindings,
                                VkDescriptorSetLayout layout,
                                cc::vector<VkSampler> immutable_samplers)
      : sg::binding_group_layout(structural_hash, cc::move(bindings)),
        _ctx(ctx),
        _layout(layout),
        _immutable_samplers(cc::move(immutable_samplers))
    {
    }

    ~vulkan_binding_group_layout() override;

    vulkan_context& _ctx;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;

    /// The VkSamplers baked into the layout as immutable ones, owned here because the layout references them for life.
    cc::vector<VkSampler> _immutable_samplers;
};
