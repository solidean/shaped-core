#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::staging_binding_group: a CPU-side image of one argument buffer.
///
/// sg owns the whole builder — name resolution, bounds checking, validation, the touched-binding demand and the
/// snapshot cache — and hands the backend four seams: write a run of view descriptors, clear a run, write one sampler,
/// and mint.
/// Everything here is those four.
///
/// **One descriptor array, not two.**
/// dx12 keeps views and samplers in separate heaps, so the base's `descriptor_offsets` speaks of "the view heap, or the
/// sampler heap for a sampler binding". An argument buffer has no such split: every binding kind is one 8-byte slot in
/// the same buffer, so both offsets are simply `binding.index`, and they cannot collide because sg already requires an
/// index to be unique within its group.
///
/// A static sampler has no slot here at all — it is written straight into the minted group from the layout — which is
/// what the base's `-1` offset means.
class sg::backend::metal::metal_staging_binding_group final : public sg::staging_binding_group
{
public:
    metal_staging_binding_group(metal_context& ctx,
                                sg::binding_group_layout_handle layout,
                                cc::vector<int> descriptor_offsets,
                                isize slot_count);

private:
    void write_view_descriptors(int first_descriptor, sg::binding const& b, cc::span<sg::raw_view const> views) override;
    void clear_view_descriptors(int first_descriptor, sg::binding const& b, int count) override;
    void write_sampler_descriptor(int descriptor_index, sg::sampler const& smp) override;
    [[nodiscard]] cc::result<sg::binding_group_handle> mint() override;

    metal_context& _ctx;

    /// The argument image: one 8-byte slot per descriptor, zero meaning vacant.
    /// A zero slot IS the null descriptor — a shader reading through it reads zeroes, which is what sg promises a
    /// vacant element does.
    cc::vector<u64> _slots;

    /// The resource each slot names, parallel to `_slots`, so a minted group can keep them alive.
    /// An argument buffer holds raw addresses and keeps nothing alive by itself.
    cc::vector<sg::raw_buffer_handle> _resources;

    /// The texture each slot names, parallel to `_slots` like `_resources`; exactly one of the two is set per slot.
    cc::vector<sg::raw_texture_handle> _texture_resources;
};
