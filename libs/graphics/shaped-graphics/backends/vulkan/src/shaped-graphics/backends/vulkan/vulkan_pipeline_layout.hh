#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>

/// The ordered set of group layouts a pipeline binds against, as a VkPipelineLayout.
///
/// A group's position in the description is its bind slot, which is the `firstSet` a bind command passes — the direct
/// analogue of dx12's root-parameter index, and rather more obvious about it.
///
/// `pipeline_layout_description::inline_constants` becomes a push-constant range.
/// Its size must be a multiple of 4, which sg validates, and Vulkan caps the total at maxPushConstantsSize.
///
/// **Each `bound_sampler` is set `sg::reserved_binding_group`, binding `index + 1`**, the address webgpu gives it too.
/// That set holds embedded immutable samplers, which live in the set layout rather than in the descriptor buffer.
/// So nothing allocates or writes a descriptor for them, and `bind_embedded_samplers` is the whole of binding them.
/// Set slots between the caller's groups and the reserved one are empty layouts, since a pipeline layout numbers its sets contiguously.
class sg::backend::vulkan::vulkan_pipeline_layout final : public sg::pipeline_layout
{
public:
    [[nodiscard]] static cc::result<vulkan_pipeline_layout_handle> create(vulkan_context& ctx,
                                                                          sg::pipeline_layout_description const& desc);

    vulkan_pipeline_layout(vulkan_context& ctx,
                           cc::hash128 structural_hash,
                           VkPipelineLayout layout,
                           cc::vector<sg::binding_group_layout_handle> groups,
                           cc::optional<sg::binding> inline_constants,
                           int inline_constants_bytes)
      : sg::pipeline_layout(structural_hash, groups, cc::move(inline_constants)),
        _ctx(ctx),
        _layout(layout),
        _groups(cc::move(groups)),
        _inline_constants_bytes(inline_constants_bytes)
    {
    }

    /// Binds the reserved set's embedded samplers, where the layout has any.
    /// Called after every pipeline bind, since binding another layout's sets may disturb set 3.
    void bind_embedded_samplers(VkCommandBuffer buffer, VkPipelineBindPoint bind_point) const;

    ~vulkan_pipeline_layout() override;

    /// Destroys the backend objects now; see sg::pipeline_layout::release_backend_objects.
    void release_backend_objects() override;

    vulkan_context& _ctx;
    VkPipelineLayout _layout = VK_NULL_HANDLE;

    /// Held so every set layout this was built from outlives it.
    cc::vector<sg::binding_group_layout_handle> _groups;

    /// Push-constant size in bytes; 0 when the description declares none.
    /// Read when validating a set_inline_constants call against what the layout actually reserved.
    int _inline_constants_bytes = 0;

    /// The reserved set's layout, and the empty ones filling the slots below it; null and empty without bound samplers.
    VkDescriptorSetLayout _reserved_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _empty_set_layout = VK_NULL_HANDLE;

    /// The immutable samplers the reserved set layout names, owned here since they must outlive it.
    cc::vector<VkSampler> _bound_samplers;
};
