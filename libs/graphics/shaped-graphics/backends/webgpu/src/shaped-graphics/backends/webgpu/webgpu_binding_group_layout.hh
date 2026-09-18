#pragma once

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
/// Refuses what WebGPU core cannot express: array bindings (`count > 1`), acceleration structures, and two bindings sharing an index.
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
                                cc::vector<cc::optional<sg::sampler>> slot_sampler_descs);

    [[nodiscard]] WGPUBindGroupLayout raw() const;

    /// Per slot in bindings(): the sampler a static sampler binding binds, null otherwise.
    [[nodiscard]] cc::span<WGPUSampler const> slot_samplers() const;

private:
    void materialize() const;

    webgpu_context& _ctx;
    cc::vector<WGPUBindGroupLayoutEntry> _entries;
    cc::vector<cc::optional<sg::sampler>> _slot_sampler_descs;

    // Made on first use, on the device thread only.
    mutable wgpu_bind_group_layout _layout;
    mutable cc::vector<WGPUSampler> _slot_samplers; // owned by the context's sampler cache
};

/// WebGPU implementation of sg::pipeline_layout.
///
/// Group 3 is sg's reserved group, and here it is where the emulated state lives:
///  - binding 0: the inline constants, a uniform buffer bound with a dynamic offset into a constant page;
///  - binding `index + 1`: each register-bound `bound_sampler`, since WebGPU has no pipeline-level samplers.
/// A bound_sampler at an index a second one already takes is refused.
///
/// Slots below the caller's groups are filled with empty layouts where group 3 exists, since WebGPU numbers groups contiguously.
///
/// Free-threaded to create, as a binding group layout is: `create` validates and describes, and the WebGPU objects are made on first use.
class sg::backend::webgpu::webgpu_pipeline_layout final : public sg::pipeline_layout
{
public:
    [[nodiscard]] static cc::result<webgpu_pipeline_layout_handle> create(webgpu_context& ctx,
                                                                          sg::pipeline_layout_description const& desc);

    webgpu_pipeline_layout(webgpu_context& ctx, cc::hash128 hash);

    [[nodiscard]] WGPUPipelineLayout raw() const;

    /// The caller's group layouts, by slot.
    [[nodiscard]] cc::span<webgpu_binding_group_layout_handle const> groups() const { return _groups; }

    /// How many slots the WebGPU layout has, filler slots and group 3 included.
    [[nodiscard]] int slot_count() const { return _slot_count; }

    /// The declared inline constants block, 0 without one.
    [[nodiscard]] isize inline_constants_bytes() const { return _inline_constants_bytes; }

    /// Whether group 3 exists at all.
    [[nodiscard]] bool has_reserved_group() const { return !_reserved_entries.empty(); }

    /// The bind group for group 3 over constant page `page`, cached per page since pages live as long as the context.
    /// Without inline constants the page is ignored and one group serves every call.
    [[nodiscard]] WGPUBindGroup reserved_group_for(webgpu_constant_page const* page) const;

    /// The empty group bound at a filler slot.
    [[nodiscard]] WGPUBindGroup empty_group() const;

private:
    void materialize() const;

    webgpu_context& _ctx;
    cc::vector<webgpu_binding_group_layout_handle> _groups;
    int _slot_count = 0;
    isize _inline_constants_bytes = 0;
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
};
