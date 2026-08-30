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
/// A binding named in the layout's static samplers gets its VkSampler created here and owned for the layout's life,
/// and every group built from the layout writes that one sampler's descriptor.
/// So the sampler state is fixed at layout creation and no caller can supply another — which is what "static" means at
/// the sg level — reached without `pImmutableSamplers`.
/// Declaring them immutable instead would leave it unclear whether the descriptor still has to be written, and buys
/// nothing a descriptor-buffer layout needs; the embedded-sampler flag that would is for sampler-only layouts.
///
/// The layout also carries the geometry a descriptor buffer needs, since a group is a byte range rather than an object:
/// how large one instance is, and where within it each binding's descriptors start.
class sg::backend::vulkan::vulkan_binding_group_layout final : public sg::binding_group_layout
{
public:
    [[nodiscard]] static cc::result<vulkan_binding_group_layout_handle>
    create(vulkan_context& ctx, cc::span<sg::binding const> bindings, cc::span<sg::named_sampler const> static_samplers);

    vulkan_binding_group_layout(vulkan_context& ctx,
                                cc::hash128 structural_hash,
                                cc::vector<sg::binding> bindings,
                                VkDescriptorSetLayout layout,
                                cc::vector<VkSampler> slot_samplers,
                                isize size_in_bytes,
                                cc::vector<isize> binding_offsets)
      : sg::binding_group_layout(structural_hash, cc::move(bindings)),
        _ctx(ctx),
        _layout(layout),
        _slot_samplers(cc::move(slot_samplers)),
        _size_in_bytes(size_in_bytes),
        _binding_offsets(cc::move(binding_offsets))
    {
    }

    ~vulkan_binding_group_layout() override;

    /// Destroys the backend objects now; see sg::binding_group_layout::release_backend_objects.
    void release_backend_objects() override;

    /// Byte offset of element `element` of binding slot `slot` within one group's range.
    /// `slot` indexes `bindings()`, and `element` must be inside that binding's count.
    [[nodiscard]] isize descriptor_offset_of(isize slot, int element) const;

    vulkan_context& _ctx;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;

    /// One entry per binding slot, parallel to `bindings()`: that slot's static sampler, or null where it has none.
    cc::vector<VkSampler> _slot_samplers;

    /// How many bytes one group built from this layout occupies in the descriptor heap.
    isize _size_in_bytes = 0;

    /// Byte offset of each binding's first descriptor within that range, parallel to `bindings()`.
    /// Element `i` of an array binding follows at `i * descriptor_size_of(type)`.
    cc::vector<isize> _binding_offsets;
};
