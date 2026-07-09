#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/binding.hh> // sg::is_sampler

namespace sg::backend::dx12
{
cc::result<dx12_pipeline_layout_handle> dx12_pipeline_layout::create(ID3D12Device* device,
                                                                     cc::span<sg::binding_group_layout_handle const> groups,
                                                                     cc::span<sg::bound_sampler const> static_samplers,
                                                                     cc::optional<sg::binding> const& inline_constants)
{
    if (int(groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    auto pl = std::make_shared<dx12_pipeline_layout>();

    // One descriptor-table root parameter per group table, appended in group order (resource table then
    // sampler table). The group layouts' range arrays back pDescriptorRanges and must outlive serialization
    // below — they do, since pl holds each group layout alive.
    cc::vector<D3D12_ROOT_PARAMETER> params;
    cc::vector<D3D12_STATIC_SAMPLER_DESC> static_sampler_descs;

    auto const add_table = [&](cc::span<D3D12_DESCRIPTOR_RANGE const> ranges)
    {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // compute uses ALL
        param.DescriptorTable.NumDescriptorRanges = UINT(ranges.size());
        param.DescriptorTable.pDescriptorRanges = ranges.data();
        int const index = int(params.size());
        params.push_back(param);
        return index;
    };

    for (auto const& g : groups)
    {
        auto const gl = std::dynamic_pointer_cast<dx12_binding_group_layout const>(g);
        CC_ASSERT(gl != nullptr, "binding_group_layout is not a dx12 binding_group_layout");

        group_slot slot;
        slot.layout = gl;
        if (!gl->view_ranges.empty())
            slot.resource_root_param = add_table(gl->view_ranges);
        if (!gl->sampler_ranges.empty())
            slot.sampler_root_param = add_table(gl->sampler_ranges);
        for (auto const& ss : gl->static_sampler_descs)
            static_sampler_descs.push_back(ss);
        pl->groups.push_back(cc::move(slot));
    }

    // Pipeline-level static samplers (register-bound, independent of the group layouts) bake in too.
    for (auto const& bs : static_samplers)
    {
        CC_ASSERT(sg::is_sampler(bs.binding.type), "pipeline_layout static sampler binding must be a sampler");
        for (int i = 0; i < int(bs.binding.count); ++i)
            static_sampler_descs.push_back(to_d3d12_static_sampler_desc(bs.sampler, UINT(bs.binding.index) + UINT(i),
                                                                        bs.binding.set, D3D12_SHADER_VISIBILITY_ALL));
    }

    // Inline constants become a 32-bit-constants root parameter, appended last so the group slots' root-
    // parameter indices above stay put. The command list addresses it via inline_constants_root_param.
    if (inline_constants.has_value())
    {
        auto const& ic = inline_constants.value();
        CC_ASSERT(ic.type == sg::binding_type::uniform_buffer, "inline_constants binding must be a uniform_buffer");
        CC_ASSERT(ic.block_size.has_value(), "inline_constants binding must have a block_size");
        CC_ASSERT(ic.block_size.value() > 0 && ic.block_size.value() % 4 == 0, "inline_constants block_size must be "
                                                                               "positive and a multiple of 4");

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // compute uses ALL
        param.Constants.Num32BitValues = UINT(ic.block_size.value() / 4);
        param.Constants.ShaderRegister = ic.index;
        param.Constants.RegisterSpace = ic.set;
        pl->inline_constants_root_param = int(params.size());
        pl->inline_constants_num_32bit = int(ic.block_size.value() / 4);
        params.push_back(param);
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = UINT(params.size());
    desc.pParameters = params.empty() ? nullptr : params.data();
    desc.NumStaticSamplers = UINT(static_sampler_descs.size());
    desc.pStaticSamplers = static_sampler_descs.empty() ? nullptr : static_sampler_descs.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    if (HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error); FAILED(hr))
        return dx12_error(hr, "D3D12SerializeRootSignature failed");

    if (HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&pl->root_signature));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateRootSignature failed");

    return dx12_pipeline_layout_handle(cc::move(pl));
}
} // namespace sg::backend::dx12
