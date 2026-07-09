#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pipeline_layout.hh>

namespace sg::backend::dx12
{
/// dx12 pipeline_layout: the ID3D12RootSignature composed from an ordered list of binding_group_layouts.
/// Each group contributes its CBV/SRV/UAV and/or SAMPLER descriptor table(s) at its own root-parameter
/// slot, and its static samplers are baked in. `groups[set]` gives the root-parameter indices the command
/// list binds a group at slot `set` to.
class dx12_pipeline_layout final : public sg::pipeline_layout
{
public:
    /// One group slot: the group layout bound here plus the root-parameter indices of its descriptor
    /// tables (-1 when the group has no view / no dynamic-sampler bindings respectively).
    struct group_slot
    {
        dx12_binding_group_layout_handle layout;
        int resource_root_param = -1;
        int sampler_root_param = -1;
    };

    /// Builds the root signature from `groups` (each an sg::binding_group_layout_handle downcast to the
    /// dx12 type). At most sg::max_binding_groups groups. `inline_constants`, when present, adds a
    /// 32-bit-constants root parameter (dx12 root constants); its binding must be a uniform_buffer with
    /// a block_size that is set and a multiple of 4.
    [[nodiscard]] static cc::result<dx12_pipeline_layout_handle> create(
        ID3D12Device* device,
        cc::span<sg::binding_group_layout_handle const> groups,
        cc::span<sg::bound_sampler const> static_samplers,
        cc::optional<sg::binding> const& inline_constants);

    ComPtr<ID3D12RootSignature> root_signature;
    cc::vector<group_slot> groups; // one per bind slot, in order

    int inline_constants_root_param = -1; ///< root-parameter index of the 32-bit-constants param, -1 if none
    int inline_constants_num_32bit = 0;   ///< block_size / 4, for full-replace size validation
};
} // namespace sg::backend::dx12
