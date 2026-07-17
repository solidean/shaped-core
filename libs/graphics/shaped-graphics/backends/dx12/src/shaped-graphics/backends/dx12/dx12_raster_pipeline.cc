#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/dx12/dx12_format.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_state.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raster_pipeline.hh>

namespace sg::backend::dx12
{
namespace
{
D3D12_DEPTH_STENCILOP_DESC to_stencil_face(sg::stencil_face const& f)
{
    D3D12_DEPTH_STENCILOP_DESC d = {};
    d.StencilFailOp = to_d3d12_stencil_op(f.fail);
    d.StencilDepthFailOp = to_d3d12_stencil_op(f.depth_fail);
    d.StencilPassOp = to_d3d12_stencil_op(f.pass);
    d.StencilFunc = to_d3d12_comparison(f.compare);
    return d;
}
} // namespace

cc::result<dx12_raster_pipeline_handle> dx12_raster_pipeline::create(ID3D12Device* device,
                                                                     dx12_pipeline_layout_handle layout,
                                                                     sg::raster_pipeline_description const& desc)
{
    CC_ASSERT(layout != nullptr, "raster pipeline requires a pipeline_layout");
    CC_ASSERT(desc.vertex_shader.stage == sg::shader_stage::vertex, "raster pipeline requires a vertex shader");
    CC_ASSERT(desc.vertex_shader.format == sg::shader_format::dxil, "the dx12 backend requires DXIL bytecode");
    CC_ASSERT(!desc.vertex_shader.bytecode.empty(), "vertex shader has no bytecode");
    if (desc.fragment_shader.has_value())
    {
        CC_ASSERT(desc.fragment_shader.value().stage == sg::shader_stage::fragment, "fragment_shader must be a "
                                                                                    "fragment shader");
        CC_ASSERT(desc.fragment_shader.value().format == sg::shader_format::dxil, "the dx12 backend requires DXIL "
                                                                                  "bytecode");
    }

    // Tessellation: hull + domain come as a pair, drive a patch-list topology, and vice versa.
    bool const has_tess = desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value();
    if (has_tess)
    {
        CC_ASSERT(desc.tessellation_control_shader.has_value() && desc.tessellation_evaluation_shader.has_value(),
                  "tessellation needs both a control (hull) and an evaluation (domain) shader");
        CC_ASSERT(desc.tessellation_control_shader.value().stage == sg::shader_stage::tessellation_control,
                  "tessellation_control_shader must be a tessellation_control stage");
        CC_ASSERT(desc.tessellation_evaluation_shader.value().stage == sg::shader_stage::tessellation_evaluation,
                  "tessellation_evaluation_shader must be a tessellation_evaluation stage");
        CC_ASSERT(desc.tessellation_control_shader.value().format == sg::shader_format::dxil
                      && desc.tessellation_evaluation_shader.value().format == sg::shader_format::dxil,
                  "the dx12 backend requires DXIL bytecode");
        CC_ASSERT(desc.topology == sg::primitive_topology::patch_list, "a tessellation pipeline requires patch_list "
                                                                       "topology");
    }
    if (desc.topology == sg::primitive_topology::patch_list)
    {
        CC_ASSERT(has_tess, "patch_list topology requires tessellation (hull + domain) shaders");
        CC_ASSERT(desc.patch_control_points >= 1 && desc.patch_control_points <= 32, "patch_control_points must be "
                                                                                     "1..32 for a patch_list pipeline");
    }
    if (desc.geometry_shader.has_value())
    {
        CC_ASSERT(desc.geometry_shader.value().stage == sg::shader_stage::geometry, "geometry_shader must be a "
                                                                                    "geometry stage");
        CC_ASSERT(desc.geometry_shader.value().format == sg::shader_format::dxil, "the dx12 backend requires DXIL "
                                                                                  "bytecode");
    }

    CC_ASSERT(desc.color_targets.size() <= 8, "a raster pipeline supports at most 8 color targets");
    CC_ASSERT(desc.sample_count >= 1, "sample_count must be >= 1");

    // The concrete IA topology set at bind time — patch lists also encode the control-point count.
    D3D12_PRIMITIVE_TOPOLOGY const ia_topology = desc.topology == sg::primitive_topology::patch_list
                                                   ? to_d3d12_patch_topology(desc.patch_control_points)
                                                   : to_d3d12_topology(desc.topology);
    auto pipeline = std::make_shared<dx12_raster_pipeline>(ia_topology);

    // Input layout — the semantic names must stay alive through CreateGraphicsPipelineState, so
    // materialize null-terminated copies into a reserved (non-reallocating) buffer and point at them.
    cc::vector<cc::string> semantic_storage;
    semantic_storage.reserve(desc.vertex_input.attributes.size());
    cc::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
    input_elements.reserve(desc.vertex_input.attributes.size());
    for (auto const& attr : desc.vertex_input.attributes)
    {
        CC_ASSERT(attr.slot >= 0 && attr.slot < int(desc.vertex_input.slots.size()), "vertex attribute slot out of "
                                                                                     "range");
        bool const per_instance = desc.vertex_input.slots[attr.slot].per_instance;

        semantic_storage.push_back(cc::string::create_copy_c_str_materialized(attr.semantic));

        D3D12_INPUT_ELEMENT_DESC e = {};
        e.SemanticName = semantic_storage.back().c_str_if_terminated();
        e.SemanticIndex = UINT(attr.semantic_index);
        e.Format = to_dxgi_vertex_format(attr.format);
        e.InputSlot = UINT(attr.slot);
        e.AlignedByteOffset = UINT(attr.offset);
        e.InputSlotClass
            = per_instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = per_instance ? 1u : 0u;
        input_elements.push_back(e);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = layout->root_signature.Get();
    pso.VS = {desc.vertex_shader.bytecode.data(), SIZE_T(desc.vertex_shader.bytecode.size())};
    if (desc.fragment_shader.has_value())
        pso.PS = {desc.fragment_shader.value().bytecode.data(), SIZE_T(desc.fragment_shader.value().bytecode.size())};
    if (desc.tessellation_control_shader.has_value())
        pso.HS = {desc.tessellation_control_shader.value().bytecode.data(),
                  SIZE_T(desc.tessellation_control_shader.value().bytecode.size())};
    if (desc.tessellation_evaluation_shader.has_value())
        pso.DS = {desc.tessellation_evaluation_shader.value().bytecode.data(),
                  SIZE_T(desc.tessellation_evaluation_shader.value().bytecode.size())};
    if (desc.geometry_shader.has_value())
        pso.GS = {desc.geometry_shader.value().bytecode.data(), SIZE_T(desc.geometry_shader.value().bytecode.size())};

    pso.InputLayout = {input_elements.empty() ? nullptr : input_elements.data(), UINT(input_elements.size())};

    // Blend — per-RT (IndependentBlendEnable), so each target's blend / write-mask applies on its own.
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = TRUE;
    for (int i = 0; i < int(desc.color_targets.size()); ++i)
    {
        auto const& ct = desc.color_targets[i];
        auto& rt = pso.BlendState.RenderTarget[i];
        rt.BlendEnable = ct.blend.has_value() ? TRUE : FALSE;
        rt.LogicOpEnable = FALSE;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        if (ct.blend.has_value())
        {
            auto const& b = ct.blend.value();
            rt.SrcBlend = to_d3d12_blend(b.color.source);
            rt.DestBlend = to_d3d12_blend(b.color.target);
            rt.BlendOp = to_d3d12_blend_op(b.color.op);
            rt.SrcBlendAlpha = to_d3d12_blend(b.alpha.source);
            rt.DestBlendAlpha = to_d3d12_blend(b.alpha.target);
            rt.BlendOpAlpha = to_d3d12_blend_op(b.alpha.op);
        }
        else
        {
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_ZERO;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        rt.RenderTargetWriteMask = to_d3d12_color_write_mask(ct.write_mask);
    }

    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = to_d3d12_fill_mode(desc.rasterization.fill);
    pso.RasterizerState.CullMode = to_d3d12_cull_mode(desc.rasterization.cull);
    pso.RasterizerState.FrontCounterClockwise
        = desc.rasterization.front == sg::front_face::counter_clockwise ? TRUE : FALSE;
    pso.RasterizerState.DepthBias = INT(desc.rasterization.depth_bias);
    pso.RasterizerState.DepthBiasClamp = desc.rasterization.depth_bias_clamp;
    pso.RasterizerState.SlopeScaledDepthBias = desc.rasterization.depth_bias_slope;
    pso.RasterizerState.DepthClipEnable = desc.rasterization.depth_clip_enabled ? TRUE : FALSE;
    pso.RasterizerState.MultisampleEnable = desc.sample_count > 1 ? TRUE : FALSE;
    pso.RasterizerState.AntialiasedLineEnable = FALSE;
    pso.RasterizerState.ForcedSampleCount = 0;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    auto const& ds = desc.depth_stencil;
    pso.DepthStencilState.DepthEnable = ds.depth_test ? TRUE : FALSE;
    pso.DepthStencilState.DepthWriteMask = ds.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = to_d3d12_comparison(ds.depth_compare);
    pso.DepthStencilState.StencilEnable = ds.stencil_test ? TRUE : FALSE;
    pso.DepthStencilState.StencilReadMask = ds.stencil_read_mask;
    pso.DepthStencilState.StencilWriteMask = ds.stencil_write_mask;
    pso.DepthStencilState.FrontFace = to_stencil_face(ds.front);
    pso.DepthStencilState.BackFace = to_stencil_face(ds.back);

    pso.PrimitiveTopologyType = to_d3d12_topology_type(sg::topology_type(desc.topology));
    pso.NumRenderTargets = UINT(desc.color_targets.size());
    for (int i = 0; i < int(desc.color_targets.size()); ++i)
        pso.RTVFormats[i] = to_dxgi_format(desc.color_targets[i].format);
    pso.DSVFormat = to_dxgi_format(desc.depth_stencil_format);
    pso.SampleDesc.Count = UINT(desc.sample_count);
    pso.SampleDesc.Quality = 0;
    pso.NodeMask = 0;
    pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    auto const cached = desc.cached_pipeline.span();
    if (!cached.empty())
    {
        pso.CachedPSO.pCachedBlob = cached.data();
        pso.CachedPSO.CachedBlobSizeInBytes = SIZE_T(cached.size());
    }

    HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline->pipeline_state));

    // A stale/mismatched blob fails with E_INVALIDARG; the cached PSO is a best-effort accelerator, so
    // degrade to a fresh build rather than hard-failing.
    if (FAILED(hr) && !cached.empty())
    {
        pso.CachedPSO = {};
        hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline->pipeline_state));
    }

    if (FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateGraphicsPipelineState failed");

    pipeline->layout = cc::move(layout);
    return dx12_raster_pipeline_handle(cc::move(pipeline));
}

cc::pinned_data<cc::byte const> dx12_raster_pipeline::cached_pipeline_data() const
{
    ComPtr<ID3DBlob> blob;
    if (FAILED(pipeline_state->GetCachedBlob(&blob)) || blob->GetBufferSize() == 0)
        return {};

    auto const bytes = cc::span<cc::byte const>(static_cast<cc::byte const*>(blob->GetBufferPointer()),
                                                cc::isize(blob->GetBufferSize()));
    return cc::pinned_data<cc::byte>::create_copy_of(bytes);
}
} // namespace sg::backend::dx12
