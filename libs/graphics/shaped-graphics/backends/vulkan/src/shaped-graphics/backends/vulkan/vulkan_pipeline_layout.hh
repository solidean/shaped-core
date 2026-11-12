#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_descriptor_heap.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

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
/// The samplers themselves are the context's sampler cache's, not the layout's.
/// So nothing allocates or writes a descriptor for them, and binding the embedded samplers is the whole of binding set 3.
///
/// With shader diagnostics on, set 3 also holds the context's debug window at binding 19, which an embedded-sampler set
/// cannot: it is then an ordinary descriptor-buffer set, its samplers written as descriptors beside the window's.
/// One region per window the layout has met, written on first use and bound with the window of the recording epoch.
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
                           int inline_constants_bytes,
                           bool dispatch_grid,
                           cc::span<sg::bound_sampler const> static_samplers,
                           sg::mesh_polyfill_slots mesh_polyfill = sg::mesh_polyfill_slots::none)
      : sg::pipeline_layout(structural_hash, groups, cc::move(inline_constants), dispatch_grid, static_samplers, mesh_polyfill),
        _ctx(ctx),
        _layout(layout),
        _groups(cc::move(groups)),
        _inline_constants_bytes(inline_constants_bytes)
    {
    }

    /// Binds set 3: its embedded samplers, or with diagnostics the region of its samplers and `debug_window`.
    /// Called after every pipeline bind, since binding another layout's sets may disturb set 3; safe from any recording thread.
    void bind_reserved(VkCommandBuffer buffer,
                       VkPipelineBindPoint bind_point,
                       sg::raw_buffer_handle const& debug_window) const;

    /// Binds set 3 of a layout whose set 3 changes per op, for one op: a transient region of its samplers, the debug
    /// window, and where they are held, the pool and `roots`, the unbounded arrays' `lengths` and the `grid` of a
    /// dispatch by threads.
    void bind_software_trace(VkCommandBuffer buffer,
                             VkPipelineBindPoint bind_point,
                             sg::raw_buffer_handle const& debug_window,
                             sg::impl::software_pool_storage const& pool,
                             cc::span<u32 const> roots,
                             cc::span<u32 const> lengths,
                             cc::span<u32 const> grid,
                             cc::span<u32 const> addresses,
                             VkDeviceAddress mesh_slots = 0,
                             isize mesh_slots_bytes = 0,
                             VkDeviceAddress task_slots = 0,
                             isize task_slots_bytes = 0) const;

    /// Whether set 3 holds values of the op, so each op binds it (bind_software_trace) rather than the pipeline bind.
    [[nodiscard]] bool binds_set_per_op() const
    {
        return _traces_in_software || _has_array_lengths || has_dispatch_grid() || _traces_by_address
            || mesh_polyfill() != sg::mesh_polyfill_slots::none;
    }

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

    /// With diagnostics, set 3 as a descriptor-buffer set: its size, each sampler and where its descriptor goes, and
    /// where the debug window's goes.
    bool _reserved_has_regions = false;
    isize _reserved_size = 0;
    cc::vector<VkSampler> _reserved_samplers;
    cc::vector<isize> _reserved_sampler_offsets;
    isize _debug_window_offset = 0;

    /// A group's binding is traced in software, so set 3 also holds the pool and the roots.
    bool _traces_in_software = false;
    isize _pool_offset = 0;
    isize _roots_offset = 0;
    /// Each group's first acceleration member among the roots and the addresses.
    cc::vector<isize> _acceleration_bases;
    /// A group holds an acceleration member, so set 3 also holds every one's address, which SGL's own SPIR-V traces.
    bool _traces_by_address = false;
    isize _addresses_offset = 0;
    /// A group holds an unbounded binding array, so set 3 also holds their lengths, each group's at its position.
    bool _has_array_lengths = false;
    isize _lengths_offset = 0;
    isize _grid_offset = 0;
    /// Where a polyfilled mesh stage's slot buffer's descriptor goes, at the binding its `mesh_polyfill` names.
    isize _mesh_slots_offset = 0;
    /// The same of a polyfilled task stage's records.
    isize _task_slots_offset = 0;
    bool _holds_mesh_slots = false;
    bool _holds_task_slots = false;
    /// Each group's position among the lengths, -1 for a group with none.
    cc::vector<isize> _length_bases;

    void write_storage_descriptor(VkDeviceAddress address, isize size_in_bytes, byte* at) const;

    struct reserved_region
    {
        sg::raw_buffer const* window = nullptr;
        vulkan_descriptor_range range;
    };
    mutable cc::mutex<cc::vector<reserved_region>> _reserved_regions;
};
