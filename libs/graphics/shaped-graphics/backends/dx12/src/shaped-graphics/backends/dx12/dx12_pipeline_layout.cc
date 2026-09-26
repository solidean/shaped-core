#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/binding/binding.hh> // sg::is_sampler
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::dx12
{
cc::result<dx12_pipeline_layout_handle> dx12_pipeline_layout::create(ID3D12Device* device,
                                                                     cc::span<sg::binding_group_layout_handle const> groups,
                                                                     cc::span<sg::bound_sampler const> static_samplers,
                                                                     cc::optional<sg::binding> const& inline_constants)
{
    if (int(groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    auto pl = std::make_shared<dx12_pipeline_layout>(
        sg::impl::pipeline_layout_hash(groups, static_samplers, inline_constants), groups, inline_constants);

    // One descriptor-table root parameter per group table, appended in group order (resource table then
    // sampler table).
    // The ranges are copied, because a range whose binding states no space takes its group's slot here, and a group
    // layout is shared by every pipeline layout that places it, at whatever slot.
    // Each group gets its own vector, reserved up front: pDescriptorRanges points into it until serialization below.
    cc::vector<D3D12_ROOT_PARAMETER> params;
    cc::vector<D3D12_STATIC_SAMPLER_DESC> static_sampler_descs;
    cc::vector<cc::vector<D3D12_DESCRIPTOR_RANGE>> placed_ranges;
    placed_ranges.reserve(2 * groups.size());

    auto const add_table = [&](cc::span<D3D12_DESCRIPTOR_RANGE const> ranges, UINT group_slot_index)
    {
        auto& placed = placed_ranges.emplace_back();
        placed.push_back_range(ranges);
        for (auto& r : placed)
            if (r.RegisterSpace == dx12_binding_group_layout::space_of_slot)
                r.RegisterSpace = group_slot_index;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // compute uses ALL
        param.DescriptorTable.NumDescriptorRanges = UINT(placed.size());
        param.DescriptorTable.pDescriptorRanges = placed.data();
        int const index = int(params.size());
        params.push_back(param);
        return index;
    };

    for (auto const& g : groups)
    {
        auto const gl = std::dynamic_pointer_cast<dx12_binding_group_layout const>(g);
        CC_ASSERT(gl != nullptr, "binding_group_layout is not a dx12 binding_group_layout");
        auto const slot_index = UINT(pl->groups.size());

        group_slot slot;
        slot.layout = gl;
        if (!gl->view_ranges.empty())
            slot.resource_root_param = add_table(gl->view_ranges, slot_index);
        if (!gl->sampler_ranges.empty())
            slot.sampler_root_param = add_table(gl->sampler_ranges, slot_index);
        for (auto ss : gl->static_sampler_descs)
        {
            if (ss.RegisterSpace == dx12_binding_group_layout::space_of_slot)
                ss.RegisterSpace = slot_index;
            static_sampler_descs.push_back(ss);
        }
        pl->groups.push_back(cc::move(slot));
    }

    // Pipeline-level static samplers (register-bound, independent of the group layouts) bake in too.
    for (auto const& bs : static_samplers)
    {
        CC_ASSERT(sg::is_sampler(bs.binding.type), "pipeline_layout static sampler binding must be a sampler");
        CC_ASSERT(bs.binding.space.has_value(), "dx12 needs an explicit register space (absent != space 0)");
        for (int i = 0; i < int(bs.binding.count); ++i)
            static_sampler_descs.push_back(to_d3d12_static_sampler_desc(
                bs.sampler, UINT(bs.binding.index) + UINT(i), bs.binding.space.value(), D3D12_SHADER_VISIBILITY_ALL));
    }

    // Inline constants become a 32-bit-constants root parameter, appended last so the group slots' root-parameter indices above stay put.
    // The command list addresses it via inline_constants_root_param.
    if (inline_constants.has_value())
    {
        auto const& ic = inline_constants.value();
        CC_ASSERT(ic.type == sg::binding_type::constants_buffer, "inline_constants binding must be a constants_buffer");
        CC_ASSERT(ic.space.has_value(), "dx12 needs an explicit register space (absent != space 0)");
        CC_ASSERT(ic.block_size.has_value(), "inline_constants binding must have a block_size");
        CC_ASSERT(ic.block_size.value() > 0 && ic.block_size.value() % 4 == 0, "inline_constants block_size must be "
                                                                               "positive and a multiple of 4");

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // compute uses ALL
        param.Constants.Num32BitValues = UINT(ic.block_size.value() / 4);
        param.Constants.ShaderRegister = ic.index;
        param.Constants.RegisterSpace = ic.space.value();
        pl->inline_constants_root_param = int(params.size());
        pl->inline_constants_num_32bit = int(ic.block_size.value() / 4);
        params.push_back(param);
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = UINT(params.size());
    desc.pParameters = params.empty() ? nullptr : params.data();
    desc.NumStaticSamplers = UINT(static_sampler_descs.size());
    desc.pStaticSamplers = static_sampler_descs.empty() ? nullptr : static_sampler_descs.data();
    // Allow the input assembler so a graphics PSO with a vertex-input layout can use this root signature
    // (required by CreateGraphicsPipelineState); the flag is inert for compute / ray-tracing pipelines.
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
