#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>

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
                                                                   cc::span<sg::binding const> bindings)
{
    auto layout = std::make_shared<dx12_binding_layout>();

    // One descriptor range per binding, packed contiguously into a single table.
    cc::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    UINT offset = 0;
    for (auto const& b : bindings)
    {
        CC_ASSERT(b.count >= 1, "unbounded / zero-count bindings are not supported yet");

        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = range_type_of(b.type);
        range.NumDescriptors = b.count;
        range.BaseShaderRegister = b.index; // (set, index) -> (space, register); register-type from the kind
        range.RegisterSpace = b.set;
        range.OffsetInDescriptorsFromTableStart = offset;
        ranges.push_back(range);

        layout->slots.push_back({b, offset});
        offset += b.count;
    }
    layout->descriptor_count = offset;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // compute uses ALL
    param.DescriptorTable.NumDescriptorRanges = UINT(ranges.size());
    param.DescriptorTable.pDescriptorRanges = ranges.data();

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = ranges.empty() ? 0 : 1;
    desc.pParameters = ranges.empty() ? nullptr : &param;
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
