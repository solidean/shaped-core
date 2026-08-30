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
class sg::backend::vulkan::vulkan_pipeline_layout final : public sg::pipeline_layout
{
public:
    [[nodiscard]] static cc::result<vulkan_pipeline_layout_handle> create(vulkan_context& ctx,
                                                                          sg::pipeline_layout_description const& desc);

    vulkan_pipeline_layout(vulkan_context& ctx,
                           cc::hash128 structural_hash,
                           VkPipelineLayout layout,
                           cc::vector<sg::binding_group_layout_handle> groups,
                           cc::vector<VkSampler> static_samplers,
                           int inline_constants_bytes)
      : sg::pipeline_layout(structural_hash),
        _ctx(ctx),
        _layout(layout),
        _groups(cc::move(groups)),
        _static_samplers(cc::move(static_samplers)),
        _inline_constants_bytes(inline_constants_bytes)
    {
    }

    ~vulkan_pipeline_layout() override;

    /// Destroys the backend objects now; see sg::pipeline_layout::release_backend_objects.
    void release_backend_objects() override;

    vulkan_context& _ctx;
    VkPipelineLayout _layout = VK_NULL_HANDLE;

    /// Held so every set layout this was built from outlives it.
    cc::vector<sg::binding_group_layout_handle> _groups;

    /// Pipeline-level static samplers, owned here for the same reason the group layout owns its own.
    cc::vector<VkSampler> _static_samplers;

    /// Push-constant size in bytes; 0 when the description declares none.
    /// Read when validating a set_inline_constants call against what the layout actually reserved.
    int _inline_constants_bytes = 0;
};
