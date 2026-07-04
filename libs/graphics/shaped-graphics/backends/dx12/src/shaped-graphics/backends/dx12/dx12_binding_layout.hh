#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_layout.hh>

namespace sg::backend::dx12
{
/// dx12 binding_layout: a compute root signature with a single CBV/SRV/UAV descriptor table — one
/// range per binding. Keeps the reflected bindings and their offsets in the table so a binding_group
/// can place and validate descriptors.
class dx12_binding_layout final : public sg::binding_layout
{
public:
    /// A reflected binding plus where its descriptors sit in the group's descriptor table.
    struct slot
    {
        sg::binding binding;
        UINT table_offset = 0;
    };

    [[nodiscard]] static cc::result<dx12_binding_layout_handle> create(ID3D12Device* device,
                                                                       cc::span<sg::binding const> bindings);

    ComPtr<ID3D12RootSignature> root_signature;
    cc::vector<slot> slots;    // one per binding, in declaration order
    UINT descriptor_count = 0; // total descriptors the table holds
};
} // namespace sg::backend::dx12
