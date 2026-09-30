#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>

/// WebGPU implementation of sg::binding_group_layout: one WGPUBindGroupLayout.
///
/// A name-matched static sampler stays an ordinary sampler entry in its own group, and the group inserts the sampler object when it is created.
/// WebGPU has no static samplers at all, so a caller never supplies one and the layout still owns what is bound there.
///
/// Refuses what WebGPU core cannot express: array bindings (`count > 1`) and two bindings sharing an index.
///
/// **An acceleration structure takes no entry**: the ray-query polyfill traces a root in the pool, which group 3 carries.
/// It keeps its slot in bindings(), so every other member's index is what it is on the other backends.
///
/// Free-threaded to create: everything sg decides is decided in `create`, and the WebGPU objects are made on first use, which is always on the device thread.
class sg::backend::webgpu::webgpu_binding_group_layout final : public sg::binding_group_layout
{
public:
    [[nodiscard]] static cc::result<webgpu_binding_group_layout_handle>
    create(webgpu_context& ctx, cc::span<sg::binding const> bindings, cc::span<sg::named_sampler const> static_samplers);

    webgpu_binding_group_layout(webgpu_context& ctx,
                                cc::hash128 hash,
                                cc::vector<sg::binding> bindings,
                                cc::vector<sg::named_sampler> static_samplers,
                                cc::vector<WGPUBindGroupLayoutEntry> entries,
                                cc::vector<cc::optional<sg::sampler>> slot_sampler_descs,
                                cc::vector<isize> acceleration_slots);

    [[nodiscard]] WGPUBindGroupLayout raw() const;

    /// The slots in bindings() of the acceleration structures, ordered by binding index, which is the order their roots take.
    [[nodiscard]] cc::span<isize const> acceleration_slots() const { return _acceleration_slots; }

    /// Per slot in bindings(): the sampler a static sampler binding binds, null otherwise.
    [[nodiscard]] cc::span<WGPUSampler const> slot_samplers() const;

private:
    void materialize() const;

    webgpu_context& _ctx;
    cc::vector<WGPUBindGroupLayoutEntry> _entries;
    cc::vector<cc::optional<sg::sampler>> _slot_sampler_descs;
    cc::vector<isize> _acceleration_slots;

    // Made on first use, on the device thread only.
    mutable wgpu_bind_group_layout _layout;
    mutable cc::vector<WGPUSampler> _slot_samplers; // owned by the context's sampler cache
};

/// WebGPU implementation of sg::pipeline_layout.
///
/// Group 3 is sg's reserved group, and here it is where the emulated state lives:
///  - binding 0: the inline constants, a uniform buffer bound with a dynamic offset into a constant page;
///  - binding `index + 1`: each register-bound `bound_sampler`, since WebGPU has no pipeline-level samplers;
///  - bindings 17 and 18, where any group holds an acceleration structure: the pool, and the roots as a uniform with a dynamic offset.
/// A bound_sampler at an index a second one already takes is refused.
///
/// The inline constants and the roots are placed as one block, so both dynamic offsets point into one constant page.
/// The roots start at the first aligned offset past the constants, one u32 per acceleration member: groups in order, members by binding index.
///
/// Slots below the caller's groups are filled with empty layouts where group 3 exists, since WebGPU numbers groups contiguously.
///
/// Free-threaded to create, as a binding group layout is: `create` validates and describes, and the WebGPU objects are made on first use.
class sg::backend::webgpu::webgpu_pipeline_layout final : public sg::pipeline_layout
{
public:
    [[nodiscard]] static cc::result<webgpu_pipeline_layout_handle> create(webgpu_context& ctx,
                                                                          sg::pipeline_layout_description const& desc);

    webgpu_pipeline_layout(webgpu_context& ctx, cc::hash128 hash, sg::pipeline_layout_description const& desc);

    [[nodiscard]] WGPUPipelineLayout raw() const;

    /// The caller's group layouts, by slot.
    [[nodiscard]] cc::span<webgpu_binding_group_layout_handle const> groups() const { return _groups; }

    /// How many slots the WebGPU layout has, filler slots and group 3 included.
    [[nodiscard]] int slot_count() const { return _slot_count; }

    /// The declared inline constants block, 0 without one.
    [[nodiscard]] isize inline_constants_bytes() const { return _inline_constants_bytes; }

    /// Whether any group holds an acceleration structure, which puts the pool and the roots in group 3.
    [[nodiscard]] bool has_acceleration() const { return _has_acceleration; }

    /// The position of group `group_index`'s first acceleration member among the roots.
    [[nodiscard]] isize acceleration_base(int group_index) const { return _acceleration_bases[group_index]; }

    /// Where the roots start in the reserved block.
    [[nodiscard]] isize roots_offset_in_block() const { return _roots_offset_in_block; }

    /// The block a list places per dispatch or draw: the inline constants, then the roots; 0 when there is neither.
    [[nodiscard]] isize reserved_block_bytes() const { return _reserved_block_bytes; }

    /// Whether group 3 exists at all.
    [[nodiscard]] bool has_reserved_group() const { return !_reserved_entries.empty(); }

    /// The bind group for group 3 over constant page `page`, cached per page since pages live as long as the context.
    /// Without a reserved block the page is ignored and one group serves every call.
    /// A grown acceleration pool drops the cache, since the group names the pool buffer.
    [[nodiscard]] WGPUBindGroup reserved_group_for(webgpu_constant_page const* page) const;

    /// Group 3's dynamic offsets for a reserved block placed at `block_offset`, in binding order, into `out`; returns how many.
    [[nodiscard]] isize reserved_dynamic_offsets(u32 block_offset, cc::span<u32> out) const;

    /// The empty group bound at a filler slot.
    [[nodiscard]] WGPUBindGroup empty_group() const;

private:
    void materialize() const;

    webgpu_context& _ctx;
    cc::vector<webgpu_binding_group_layout_handle> _groups;
    int _slot_count = 0;
    isize _inline_constants_bytes = 0;
    bool _has_acceleration = false;
    cc::vector<isize> _acceleration_bases;
    isize _roots_offset_in_block = 0;
    isize _reserved_block_bytes = 0;
    cc::vector<WGPUBindGroupLayoutEntry> _reserved_entries;

    /// Group 3's sampler entries, by binding.
    struct reserved_sampler
    {
        u32 binding = 0;
        sg::sampler desc;
        WGPUSampler sampler = nullptr; // resolved on first use
    };
    mutable cc::vector<reserved_sampler> _reserved_samplers;

    // Made on first use, on the device thread only.
    mutable wgpu_pipeline_layout _layout;
    mutable wgpu_bind_group_layout _reserved_layout;
    mutable wgpu_bind_group_layout _empty_layout;
    mutable wgpu_bind_group _empty_group;

    /// Group 3 over one page, keyed by the page's position in the context's page list.
    mutable cc::vector<wgpu_bind_group> _reserved_groups;
    mutable wgpu_bind_group _reserved_group_without_page;
    mutable u64 _reserved_groups_pool_generation = 0;
};
