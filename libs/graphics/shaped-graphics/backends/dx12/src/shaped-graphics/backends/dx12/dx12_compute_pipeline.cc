#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/compiled_shader.hh>

namespace sg::backend::dx12
{
cc::result<dx12_compute_pipeline_handle> dx12_compute_pipeline::create(ID3D12Device* device,
                                                                       dx12_pipeline_layout_handle layout,
                                                                       sg::compiled_shader const& shader)
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

    if (HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline->pipeline_state)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateComputePipelineState failed");

    return dx12_compute_pipeline_handle(cc::move(pipeline));
}
} // namespace sg::backend::dx12
