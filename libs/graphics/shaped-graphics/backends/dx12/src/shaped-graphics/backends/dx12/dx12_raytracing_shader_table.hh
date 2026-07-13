#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing_shader_table.hh>

namespace sg::backend::dx12
{
/// dx12 ray-tracing shader table: a GPU buffer of 32-byte shader-identifier records (raygen / miss / hit /
/// callable sections) copied from a dx12_raytracing_pipeline, plus the four GPU-address ranges DispatchRays
/// reads. Backed by a plain shader-readable + copy-dst buffer for now (types.hh reserves a dedicated
/// shader_binding_table usage as future work).
class dx12_raytracing_shader_table final : public sg::raytracing_shader_table
{
public:
    /// Builds the table: validates the handles against the pipeline, lays out the four sections, uploads the
    /// records, and captures the address ranges. Requires at least one raygen record.
    [[nodiscard]] static cc::result<dx12_raytracing_shader_table_handle> create(
        dx12_context& ctx,
        sg::raytracing_shader_table_description const& desc);

    /// The single raygen record at `index`, for DispatchRays' RayGenerationShaderRecord (a range, no stride).
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS_RANGE raygen_record(sg::raygen_index index) const;

    dx12_buffer_handle buffer; // backing buffer (kept alive; declared shader_read at dispatch)

    // Section address ranges; an empty section stays {} (StartAddress 0), which DispatchRays treats as unused.
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE raygen_table = {};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE miss_table = {};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hit_table = {};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE callable_table = {};

private:
    explicit dx12_raytracing_shader_table(sg::raytracing_pipeline_handle pipeline)
      : sg::raytracing_shader_table(cc::move(pipeline))
    {
    }
};
} // namespace sg::backend::dx12
