#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/compute_pipeline.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// dx12 compute pipeline: an ID3D12PipelineState compiled from a compute shader against a pipeline
/// layout's root signature. Holds the pipeline layout to keep the root signature alive and reachable at
/// bind time.
class dx12_compute_pipeline final : public sg::compute_pipeline
{
public:
    explicit dx12_compute_pipeline(sg::compute_dimensions workgroup_size) : sg::compute_pipeline(workgroup_size) {}

    [[nodiscard]] static cc::result<dx12_compute_pipeline_handle> create(ID3D12Device* device,
                                                                         dx12_pipeline_layout_handle layout,
                                                                         sg::compiled_shader const& shader,
                                                                         cc::span<cc::byte const> cached_pipeline = {});

    /// The PSO's serialized blob via ID3D12PipelineState::GetCachedBlob; empty on failure.
    [[nodiscard]] cc::pinned_data<cc::byte const> cached_pipeline_data() const override;

    dx12_pipeline_layout_handle layout;
    ComPtr<ID3D12PipelineState> pipeline_state;
};
} // namespace sg::backend::dx12
