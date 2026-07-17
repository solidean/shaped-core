#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster_pipeline.hh>

namespace sg::backend::dx12
{
/// dx12 raster pipeline: an ID3D12PipelineState compiled from a vertex (+ optional fragment) shader and
/// fixed-function state against a pipeline layout's root signature. Holds the pipeline layout to keep the
/// root signature alive, and the IA primitive topology to set at bind time.
class dx12_raster_pipeline final : public sg::raster_pipeline
{
public:
    explicit dx12_raster_pipeline(D3D12_PRIMITIVE_TOPOLOGY ia_topology) : topology(ia_topology) {}

    [[nodiscard]] static cc::result<dx12_raster_pipeline_handle> create(ID3D12Device* device,
                                                                        dx12_pipeline_layout_handle layout,
                                                                        sg::raster_pipeline_description const& desc);

    /// The PSO's serialized blob via ID3D12PipelineState::GetCachedBlob; empty on failure.
    [[nodiscard]] cc::pinned_data<cc::byte const> cached_pipeline_data() const override;

    dx12_pipeline_layout_handle layout;
    ComPtr<ID3D12PipelineState> pipeline_state;
    D3D12_PRIMITIVE_TOPOLOGY topology; // set on the IA at bind_pipeline (the PSO records only the family)
};
} // namespace sg::backend::dx12
