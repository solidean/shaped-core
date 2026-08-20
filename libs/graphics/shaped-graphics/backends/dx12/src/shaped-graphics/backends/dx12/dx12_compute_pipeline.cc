#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

namespace sg::backend::dx12
{
cc::result<dx12_compute_pipeline_handle> dx12_compute_pipeline::create(ID3D12Device* device,
                                                                       dx12_pipeline_layout_handle layout,
                                                                       sg::compiled_shader const& shader,
                                                                       cc::span<byte const> cached_pipeline)
{
    CC_ASSERT(layout != nullptr, "compute pipeline requires a pipeline_layout");
    CC_ASSERT(shader.stage == sg::shader_stage::compute, "compute pipeline requires a compute shader");
    CC_ASSERT(shader.format == sg::shader_format::dxil, "the dx12 backend requires DXIL bytecode");
    CC_ASSERT(!shader.bytecode.empty(), "compute shader has no bytecode");
    CC_ASSERT(shader.workgroup_size.has_value(), "a compute shader must report its workgroup size");

    auto pipeline = std::make_shared<dx12_compute_pipeline>(shader.workgroup_size.value());
    pipeline->layout = cc::move(layout);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = pipeline->layout->root_signature.Get();
    desc.CS.pShaderBytecode = shader.bytecode.data();
    desc.CS.BytecodeLength = SIZE_T(shader.bytecode.size());
    if (!cached_pipeline.empty())
    {
        desc.CachedPSO.pCachedBlob = cached_pipeline.data();
        desc.CachedPSO.CachedBlobSizeInBytes = SIZE_T(cached_pipeline.size());
    }

    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline->pipeline_state));
    pipeline->_used_cached_pipeline = SUCCEEDED(hr) && !cached_pipeline.empty();

    // A stale or mismatched blob (e.g. after a driver update) fails PSO creation.
    // The cached PSO is a best-effort accelerator, so any failure with a blob present degrades to a fresh build rather than hard-failing.
    //
    // Taking this path is also the ONLY exact answer to "did the driver accept our blob" — d3d12 never silently
    // ignores one — which is what used_cached_pipeline() reports.
    if (FAILED(hr) && !cached_pipeline.empty())
    {
        desc.CachedPSO = {};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline->pipeline_state));
    }

    if (FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateComputePipelineState failed");

    return dx12_compute_pipeline_handle(cc::move(pipeline));
}

cc::pinned_data<byte const> dx12_compute_pipeline::cached_pipeline_data() const
{
    ComPtr<ID3DBlob> blob;
    if (FAILED(pipeline_state->GetCachedBlob(&blob)) || blob->GetBufferSize() == 0)
        return {};

    auto const bytes
        = cc::span<byte const>(static_cast<byte const*>(blob->GetBufferPointer()), isize(blob->GetBufferSize()));
    return cc::pinned_data<byte>::create_copy_of(bytes);
}
} // namespace sg::backend::dx12
