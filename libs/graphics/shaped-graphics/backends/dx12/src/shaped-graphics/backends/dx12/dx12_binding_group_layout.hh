#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group_layout.hh>

namespace sg::backend::dx12
{
/// dx12 binding_group_layout: one group's descriptor-table schema — a CBV/SRV/UAV table (one range per
/// resource-view binding) and a SAMPLER table (one range per *dynamic* sampler binding) — plus any
/// *static* sampler descs baked from this group's bindings. It is NOT a root signature; a dx12_pipeline_layout
/// composes one or more of these into the root signature and assigns each a root-parameter slot. Keeps the
/// reflected view/sampler bindings and their table offsets so a binding_group can place and validate descriptors.
class dx12_binding_group_layout final : public sg::binding_group_layout
{
public:
    /// A reflected binding plus where its descriptor(s) sit in the group's descriptor table.
    struct slot
    {
        sg::binding binding;
        int table_offset = 0;
    };

    /// `bindings` are the shader's reflected bindings; any sampler binding named in `static_samplers` is
    /// baked into `static_sampler_descs`, the rest become dynamic sampler-table entries.
    [[nodiscard]] static cc::result<dx12_binding_group_layout_handle> create(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers);

    cc::vector<slot> view_slots; // resource-view bindings, in declaration order
    int descriptor_count = 0;    // descriptors the CBV/SRV/UAV table holds

    cc::vector<slot> sampler_slots;   // dynamic sampler bindings, in declaration order
    int sampler_descriptor_count = 0; // descriptors the SAMPLER table holds

    // Descriptor ranges (in this group's table space) + static sampler descs — assembled into the root
    // signature by dx12_pipeline_layout. The range vectors must outlive its serialization (the pipeline
    // layout holds this group layout alive, so they do).
    cc::vector<D3D12_DESCRIPTOR_RANGE> view_ranges;
    cc::vector<D3D12_DESCRIPTOR_RANGE> sampler_ranges;
    cc::vector<D3D12_STATIC_SAMPLER_DESC> static_sampler_descs;
};
} // namespace sg::backend::dx12
