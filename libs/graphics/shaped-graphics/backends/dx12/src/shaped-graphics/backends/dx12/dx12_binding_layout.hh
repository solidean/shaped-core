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
/// dx12 binding_layout: a compute root signature with up to two descriptor tables — a CBV/SRV/UAV table
/// (one range per resource-view binding) and a SAMPLER table (one range per *dynamic* sampler binding) —
/// plus any *static* samplers baked directly into the root signature. Keeps the reflected view/sampler
/// bindings and their table offsets so a binding_group can place and validate descriptors.
class dx12_binding_layout final : public sg::binding_layout
{
public:
    /// A reflected binding plus where its descriptor(s) sit in the group's descriptor table.
    struct slot
    {
        sg::binding binding;
        int table_offset = 0;
    };

    /// `bindings` are the shader's reflected bindings; any sampler binding named in `static_samplers` is
    /// baked into the root signature, the rest become dynamic sampler-table entries.
    [[nodiscard]] static cc::result<dx12_binding_layout_handle> create(ID3D12Device* device,
                                                                       cc::span<sg::binding const> bindings,
                                                                       cc::span<sg::named_sampler const> static_samplers);

    ComPtr<ID3D12RootSignature> root_signature;

    cc::vector<slot> view_slots;  // resource-view bindings, in declaration order
    int descriptor_count = 0;     // descriptors the CBV/SRV/UAV table holds
    int resource_root_param = -1; // root-parameter index of that table (-1 if there are no view bindings)

    cc::vector<slot> sampler_slots;   // dynamic sampler bindings, in declaration order
    int sampler_descriptor_count = 0; // descriptors the SAMPLER table holds
    int sampler_root_param = -1;      // root-parameter index of that table (-1 if there are no dynamic samplers)
};
} // namespace sg::backend::dx12
