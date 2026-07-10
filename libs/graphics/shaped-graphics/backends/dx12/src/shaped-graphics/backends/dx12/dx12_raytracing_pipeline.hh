#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing_pipeline.hh>

namespace sg::backend::dx12
{
/// dx12 ray-tracing pipeline: an ID3D12StateObject (DXR raytracing pipeline) assembled from the compiled
/// shader libraries, hit groups, and the pipeline layout's global root signature. Holds the pipeline layout
/// to keep the root signature alive, plus the 32-byte shader identifiers a shader table copies (indexed by
/// the matching *_shader_handle).
class dx12_raytracing_pipeline final : public sg::raytracing_pipeline
{
public:
    /// One DXR shader identifier — D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES (32) opaque bytes.
    struct shader_identifier
    {
        cc::byte bytes[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
    };

    /// Builds the state object from `desc` (its shaders + limits) against `layout`'s root signature.
    /// Requires ID3D12Device5 (queried from `device`); at least one raygen shader.
    [[nodiscard]] static cc::result<dx12_raytracing_pipeline_handle> create(ID3D12Device* device,
                                                                            dx12_pipeline_layout_handle layout,
                                                                            sg::raytracing_pipeline_description const& desc);

    /// State-object cached blobs are a later optimization — empty for now.
    [[nodiscard]] cc::pinned_data<cc::byte const> cached_pipeline_data() const override;

    dx12_pipeline_layout_handle layout;
    ComPtr<ID3D12StateObject> state_object;

    // 32-byte shader identifiers, indexed by the matching handle (raygen_shader_handle, ...). Hit identifiers
    // are for the hit *group*, not the individual closest/any/intersection shaders.
    cc::vector<shader_identifier> shader_ids_raygen;
    cc::vector<shader_identifier> shader_ids_miss;
    cc::vector<shader_identifier> shader_ids_hit;
    cc::vector<shader_identifier> shader_ids_callable;
};
} // namespace sg::backend::dx12
