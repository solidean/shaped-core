#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

/// dx12 raster pipeline: an ID3D12PipelineState compiled from a vertex (+ optional fragment) shader and fixed-function state, against a pipeline layout's root signature.
/// Holds the pipeline layout to keep the root signature alive, and the IA primitive topology to set at bind time.
class sg::backend::dx12::dx12_raster_pipeline final : public sg::raster_pipeline
{
public:
    explicit dx12_raster_pipeline(D3D12_PRIMITIVE_TOPOLOGY ia_topology) : topology(ia_topology) {}

    [[nodiscard]] static cc::result<dx12_raster_pipeline_handle> create(ID3D12Device* device,
                                                                        dx12_pipeline_layout_handle layout,
                                                                        sg::raster_pipeline_description const& desc);

    /// The PSO's serialized blob via ID3D12PipelineState::GetCachedBlob; empty on failure.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override;

    dx12_pipeline_layout_handle layout;
    ComPtr<ID3D12PipelineState> pipeline_state;
    /// For a strip, the same pipeline restarting at a 16-bit index's all-ones, where `pipeline_state` restarts at a 32-bit one.
    /// Null for every other topology, whose PSO cuts no strip.
    ComPtr<ID3D12PipelineState> pipeline_state_16;

    /// Set for a mesh-shaded or a view-instanced pipeline, which neither restores from a cached blob nor hands one out.
    /// A mesh-shaded blob is refused on every restore, a run after the one that stored it, with the debug layer reporting a description that does not match.
    /// Why is open: bug-sg-dx12-mesh-pipeline-blob-refused.
    /// A view-instanced blob crashes Intel's driver on a restore in another process: docs/bugs-external/intel-d3d12-view-instanced-cached-blob-crashes.
    bool skips_cached_blob = false;
    D3D12_PRIMITIVE_TOPOLOGY topology; // set on the IA at bind_pipeline (the PSO records only the family)
};
