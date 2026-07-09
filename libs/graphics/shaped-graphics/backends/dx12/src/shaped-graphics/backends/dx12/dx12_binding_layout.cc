#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/binding_group.hh> // sg::named_sampler
#include <shaped-graphics/sampler.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] D3D12_DESCRIPTOR_RANGE_TYPE range_type_of(sg::binding_type t)
{
    switch (sg::access_of(t))
    {
    case sg::view_class::uniform:
        return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case sg::view_class::readonly:
        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case sg::view_class::readwrite:
        return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
    CC_UNREACHABLE("unhandled binding access class");
}
} // namespace

cc::result<dx12_binding_layout_handle> dx12_binding_layout::create(ID3D12Device* device,
                                                                   cc::span<sg::binding const> bindings,
                                                                   cc::span<sg::named_sampler const> static_samplers)
{
    auto layout = std::make_shared<dx12_binding_layout>();

    // A sampler binding is *static* (baked into the root signature) iff its name appears in static_samplers;
    // otherwise it is a *dynamic* sampler-table entry supplied per binding_group.
    auto const find_static = [&](cc::string_view name) -> sg::sampler_description const*
    {
        for (auto const& ns : static_samplers)
            if (ns.name == name)
                return &ns.sampler;
        return nullptr;
    };

    // Resource views go in the CBV/SRV/UAV table; dynamic samplers in the SAMPLER table (D3D12 forbids
    // mixing them). Each table packs its bindings contiguously; static samplers sit outside both.
    cc::vector<D3D12_DESCRIPTOR_RANGE> view_ranges;
    cc::vector<D3D12_DESCRIPTOR_RANGE> sampler_ranges;
    cc::vector<D3D12_STATIC_SAMPLER_DESC> static_sampler_descs;
    int view_offset = 0;
    int sampler_offset = 0;
    int matched_static = 0;

    for (auto const& b : bindings)
    {
        CC_ASSERT(b.count >= 1, "unbounded / zero-count bindings are not supported yet");

        if (sg::is_sampler(b.type))
        {
            if (auto const* sd = find_static(b.name); sd != nullptr)
            {
                for (int i = 0; i < int(b.count); ++i)
                    static_sampler_descs.push_back(
                        to_d3d12_static_sampler_desc(*sd, UINT(b.index) + UINT(i), b.set, D3D12_SHADER_VISIBILITY_ALL));
                ++matched_static;
            }
            else
            {
                D3D12_DESCRIPTOR_RANGE range = {};
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = b.count;
                range.BaseShaderRegister = b.index;
                range.RegisterSpace = b.set;
                range.OffsetInDescriptorsFromTableStart = UINT(sampler_offset);
                sampler_ranges.push_back(range);
                layout->sampler_slots.push_back({b, sampler_offset});
                sampler_offset += int(b.count);
            }
            continue;
        }

        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = range_type_of(b.type);
        range.NumDescriptors = b.count;
        range.BaseShaderRegister = b.index; // (set, index) -> (space, register); register-type from the kind
        range.RegisterSpace = b.set;
        range.OffsetInDescriptorsFromTableStart = UINT(view_offset);
        view_ranges.push_back(range);
        layout->view_slots.push_back({b, view_offset});
        view_offset += int(b.count);
    }
    layout->descriptor_count = view_offset;
    layout->sampler_descriptor_count = sampler_offset;

    // Every named static sampler must correspond to a sampler binding (unique names assumed).
    if (matched_static != int(static_samplers.size()))
        return cc::error("binding_layout: a static sampler name matches no sampler binding");

    // Up to two descriptor-table root parameters (resource table, then sampler table), plus the baked
    // static samplers. The range arrays must outlive serialization below.
    cc::vector<D3D12_ROOT_PARAMETER> params;
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
    if (!view_ranges.empty())
        layout->resource_root_param = add_table(view_ranges);
    if (!sampler_ranges.empty())
        layout->sampler_root_param = add_table(sampler_ranges);

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
                                                 IID_PPV_ARGS(&layout->root_signature));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateRootSignature failed");

    return dx12_binding_layout_handle(cc::move(layout));
}
} // namespace sg::backend::dx12
