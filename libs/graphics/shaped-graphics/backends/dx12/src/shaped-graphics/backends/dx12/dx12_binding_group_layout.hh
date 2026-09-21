#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/fwd.hh>

/// dx12 binding_group_layout: one group's descriptor-table schema.
/// That is a CBV/SRV/UAV table (one range per resource-view binding) and a SAMPLER table (one range per *dynamic* sampler binding), plus any *static* sampler descs baked from this group's bindings.
/// It is NOT a root signature: a dx12_pipeline_layout composes one or more of these into the root signature and assigns each a root-parameter slot.
/// Keeps the reflected view/sampler bindings and their table offsets, so a binding_group can place and validate descriptors.
class sg::backend::dx12::dx12_binding_group_layout final : public sg::binding_group_layout
{
public:
    dx12_binding_group_layout(cc::hash128 structural_hash,
                              cc::vector<sg::binding> bindings,
                              cc::vector<sg::named_sampler> static_samplers)
      : sg::binding_group_layout(structural_hash, cc::move(bindings), cc::move(static_samplers))
    {
    }

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

    /// For each position in `bindings()`, where that binding sits in `view_slots` or in `sampler_slots`.
    ///
    /// A binding_slot is a position in `bindings()`, and this table is split in two, so the two indices part
    /// company as soon as a sampler interleaves with a view.
    /// -1 for a static sampler, which is in neither table: it lives in the root signature.
    cc::vector<int> slot_by_binding;

    /// The RegisterSpace of a range whose binding states none: the pipeline layout writes the group's slot in its place.
    ///
    /// A binding without a space is one that fixes no group of its own, which is every binding of an SGL group.
    /// Its register space is then where the pipeline layout puts the group, the same rule vulkan applies to a set.
    static constexpr UINT space_of_slot = ~UINT(0);

    // Descriptor ranges, in this group's table space, plus static sampler descs — assembled into the root signature by dx12_pipeline_layout.
    // A range may carry `space_of_slot`, so the pipeline layout copies them rather than pointing at these.
    cc::vector<D3D12_DESCRIPTOR_RANGE> view_ranges;
    cc::vector<D3D12_DESCRIPTOR_RANGE> sampler_ranges;
    cc::vector<D3D12_STATIC_SAMPLER_DESC> static_sampler_descs;
};
