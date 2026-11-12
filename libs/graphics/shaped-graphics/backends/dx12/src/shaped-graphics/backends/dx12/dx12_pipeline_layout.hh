#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>

/// dx12 pipeline_layout: the ID3D12RootSignature composed from an ordered list of binding_group_layouts.
/// Each group contributes its CBV/SRV/UAV and/or SAMPLER descriptor table(s) at its own root-parameter slot, and its static samplers are baked in.
/// `groups[set]` gives the root-parameter indices the command list binds a group at slot `set` to.
class sg::backend::dx12::dx12_pipeline_layout final : public sg::pipeline_layout
{
public:
    dx12_pipeline_layout(cc::hash128 structural_hash,
                         cc::span<sg::binding_group_layout_handle const> groups,
                         cc::optional<sg::binding> inline_constants,
                         bool dispatch_grid,
                         cc::span<sg::bound_sampler const> static_samplers,
                         sg::mesh_polyfill_slots mesh_polyfill = sg::mesh_polyfill_slots::none)
      : sg::pipeline_layout(structural_hash, groups, cc::move(inline_constants), dispatch_grid, static_samplers, mesh_polyfill)
    {
    }

    /// One group slot: the group layout bound here, plus the root-parameter indices of its descriptor tables.
    /// Each index is -1 when the group has no view / no dynamic-sampler bindings respectively.
    struct group_slot
    {
        dx12_binding_group_layout_handle layout;
        int resource_root_param = -1;
        int sampler_root_param = -1;
    };

    /// Builds the root signature from `groups`, each an sg::binding_group_layout_handle downcast to the dx12 type.
    /// At most sg::max_binding_groups groups.
    /// `inline_constants`, when present, adds a 32-bit-constants root parameter (dx12 root constants).
    /// Its binding must be a constants_buffer with a block_size that is set and a multiple of 4.
    [[nodiscard]] static cc::result<dx12_pipeline_layout_handle> create(
        ID3D12Device* device,
        cc::span<sg::binding_group_layout_handle const> groups,
        cc::span<sg::bound_sampler const> static_samplers,
        cc::optional<sg::binding> const& inline_constants,
        bool dispatch_grid,
        sg::mesh_polyfill_slots mesh_polyfill = sg::mesh_polyfill_slots::none);

    ComPtr<ID3D12RootSignature> root_signature;
    cc::vector<group_slot> groups; // one per bind slot, in order

    int inline_constants_root_param = -1; ///< root-parameter index of the 32-bit-constants param, -1 if none
    int inline_constants_num_32bit = 0;   ///< block_size / 4, for full-replace size validation
    int debug_root_param = -1;            ///< root-parameter index of the debug window's root UAV
    int software_pool_root_param = -1;  ///< root-parameter index of the software walk's pool, -1 unless a binding asks
    int software_roots_root_param = -1; ///< root-parameter index of its 16 root constants

    /// Each group's first acceleration member among the roots.
    cc::vector<isize> acceleration_bases;
    /// Root-parameter index of the unbounded arrays' lengths, 16 root constants, -1 unless a group holds such an array.
    int array_lengths_root_param = -1;
    /// Each group's position among those lengths, -1 for a group with no unbounded array.
    cc::vector<isize> length_bases;
    /// Root-parameter index of the grid `dispatch_threads` writes, 4 root constants, -1 unless the layout holds it.
    int dispatch_grid_root_param = -1;
    /// Root-parameter index of a polyfilled mesh stage's slots: a root UAV for its compute half, a root SRV for its
    /// vertex half; -1 where the layout holds neither.
    int mesh_slots_root_param = -1;
    /// The same of a polyfilled task stage's records: a root UAV for its compute form and scan, a root SRV for the mesh
    /// stage's compute half that reads them; -1 where the layout holds neither.
    int task_slots_root_param = -1;

    /// Whether the root signature carries a static sampler, a group's or the layout's own.
    /// **A pipeline over one neither restores from a cached blob nor hands one out.**
    /// A correct blob, restored in a process that has built the same shader under a static sampler that differs, came
    /// back sampling through that other sampler.
    /// The driver accepted the blob and reported no error; a blob stored in such a process was wrong too, and persisted.
    /// Seen on an RTX 5070 Ti with driver 32.0.15.9186; a pipeline without static samplers restores correctly there.
    /// docs/bugs-external/d3d12-cached-pso-static-sampler-mixup reproduces it standalone, and says when to retire this.
    bool has_static_samplers = false;
};
