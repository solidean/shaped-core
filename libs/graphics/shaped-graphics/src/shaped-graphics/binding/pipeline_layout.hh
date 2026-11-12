#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/reserved_bindings.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>

/// A static sampler bound directly to a shader register, for attaching to a pipeline_layout — the register-bound counterpart to a group layout's name-matched `named_sampler`.
/// Use this for a static sampler a pipeline needs on top of, or independent of, its group layouts.
/// `binding` carries the register (index), its space and count, and its `type` must be a sampler binding of count 1.
/// The register must be below `sg::max_bound_samplers`, and no two of a layout's may share one, whatever their spaces.
struct sg::bound_sampler
{
    sg::binding binding;
    sg::sampler sampler;
};

/// Which slots of a polyfilled mesh stage a pipeline layout holds (libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md).
enum class sg::mesh_polyfill_slots : sg::u8
{
    none,
    /// `sg_mesh_output`, which the compute half writes, and the grid it reads.
    output,
    /// `sg_mesh_input`, which the vertex half reads.
    input,
    /// `sg_task_output`, which a polyfilled task stage's compute form and the scan after it write, and the grid both
    /// read.
    task,
    /// `sg_mesh_output` and `sg_task_input`: the compute half of a mesh stage a polyfilled task stage launches.
    launched,
};

namespace sg::impl
{
/// One buffer a layout of a polyfill's slots holds: the mesh stage's slots, or a task stage's records.
struct polyfill_slot
{
    reserved_binding_address const* address = nullptr;
    bool is_writable = false;
    /// Read by a vertex half rather than written or read by a compute form.
    bool is_vertex = false;
    /// The task stage's records rather than the mesh stage's slots.
    bool is_task = false;
};

/// The buffers a layout of `slots` holds, in binding order; empty for `none`.
[[nodiscard]] inline cc::fixed_vector<polyfill_slot, 2> polyfill_slots_of(mesh_polyfill_slots slots)
{
    auto result = cc::fixed_vector<polyfill_slot, 2>();
    if (slots == mesh_polyfill_slots::output || slots == mesh_polyfill_slots::launched)
        result.push_back({.address = &reserved_mesh_output, .is_writable = true});
    if (slots == mesh_polyfill_slots::input)
        result.push_back({.address = &reserved_mesh_input, .is_vertex = true});
    if (slots == mesh_polyfill_slots::task)
        result.push_back({.address = &reserved_task_output, .is_writable = true, .is_task = true});
    if (slots == mesh_polyfill_slots::launched)
        result.push_back({.address = &reserved_task_input, .is_task = true});
    return result;
}
} // namespace sg::impl

/// Description for building a pipeline_layout: an ordered list of binding_group_layouts, one per bind slot, plus any extra register-bound static samplers.
/// `groups[i]` is the schema bound at slot `i` — the `group_index` of cmd.compute.bind_group.
struct sg::pipeline_layout_description
{
    // Ordered; index = bind slot.
    // Owning, and inline-capped at max_binding_groups so the common case never heap-allocates.
    // Slot sg::reserved_binding_group sits above this range and belongs to sg, so a description cannot reach it.
    // issue: chore-sg-pipeline-layout-fixed-vector
    cc::small_vector<binding_group_layout_handle, max_binding_groups> groups;

    // Extra static samplers bound directly to shader registers, not tied to any group's bindings.
    // Baked into the root signature alongside the group layouts' own name-matched static samplers.
    cc::vector<bound_sampler> static_samplers;

    /// An optional constant buffer binding that provides inline (root/push) constants.
    /// Fast per-draw parameter updates without allocating separate descriptor space, set through `cmd.compute.set_inline_constants(...)` and its raster / raytracing twins.
    /// Maps to dx12 root constants / vulkan push constants.
    ///
    /// This binding must be a constant buffer, and must be excluded from the group layouts to avoid allocation conflicts.
    /// `block_size` must be set, and a multiple of 4.
    ///
    /// Sizing guidance:
    /// - 0–64 bytes (16 floats / 1 mat4): always preferable to separate buffers.
    /// - Up to 128 bytes (32 floats / 2 mat4): acceptable but avoid changing every draw; reserve for
    ///   pass-level data or other parameters that update infrequently.
    /// - Beyond 128 bytes: usually a mistake, use inline-uploaded buffers instead.
    /// Reserve inline constants for small data that genuinely varies per invocation.
    cc::optional<binding> inline_constants;

    /// The layout holds the grid `dispatch_threads` writes, reserved `sg_dispatch_grid`, which a shader sg dispatches by
    /// threads alone reads.
    /// Pipeline creation sets it for such a shader, so a description rarely does.
    bool dispatch_grid = false;

    /// The layout holds a polyfilled mesh stage's slots: reserved `sg_mesh_output` for its compute half, or
    /// `sg_mesh_input` for its vertex half; never both, since WebGPU refuses one buffer bound writable and read-only.
    /// Pipeline creation sets it for a polyfilled pipeline's two halves, so a description rarely does.
    mesh_polyfill_slots mesh_polyfill = mesh_polyfill_slots::none;
};

/// The binding interface a pipeline is compiled against: an ordered set of binding_group_layouts.
/// Composing groups into slots lets an entire group be rebound at one slot without disturbing the others.
/// Held via pipeline_layout_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 root signature, vulkan
/// VkPipelineLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class sg::pipeline_layout
{
public:
    virtual ~pipeline_layout();

    /// Destroys the backend objects now, leaving this an inert husk.
    ///
    /// The context calls this at shutdown for everything its pipeline cache still holds, because the cache dropping
    /// its own reference is NOT what frees them: a scheduled build node keeps the handle alive on a pool worker, and
    /// that node's later drop would run this destructor against a context that no longer exists.
    /// See libs/graphics/shaped-graphics/docs/concepts/caches.md, "Shutdown releases the objects, not just the entries".
    ///
    /// Idempotent, and the destructor afterwards does nothing.
    /// The default is empty, for a backend whose objects are reference-counted and safe to drop late.
    virtual void release_backend_objects() {}

    /// Content identity: a hash over the group layouts' own identities, the static samplers and the inline constants.
    /// Stable across processes, which is what lets it key a cache that outlives one.
    ///
    /// Not virtual on purpose: identity is decided at the sg level, from sg-level inputs, so two backends cannot
    /// disagree about which layouts are the same one.
    [[nodiscard]] cc::hash128 structural_hash() const { return _structural_hash; }

    /// The group layout at each bind slot, as the description gave them.
    [[nodiscard]] cc::span<binding_group_layout_handle const> groups() const { return _groups; }

    /// The description's inline constants binding, if it had one.
    [[nodiscard]] cc::optional<binding> const& inline_constants() const { return _inline_constants; }

    /// Whether the layout holds the grid `dispatch_threads` writes (`pipeline_layout_description::dispatch_grid`).
    [[nodiscard]] bool has_dispatch_grid() const { return _has_dispatch_grid; }

    /// Which of a polyfilled mesh stage's slots the layout holds (`pipeline_layout_description::mesh_polyfill`).
    [[nodiscard]] mesh_polyfill_slots mesh_polyfill() const { return _mesh_polyfill; }

    /// The description it was created from, which creates it again.
    [[nodiscard]] pipeline_layout_description description() const
    {
        auto result = pipeline_layout_description{.inline_constants = _inline_constants,
                                                  .dispatch_grid = _has_dispatch_grid,
                                                  .mesh_polyfill = _mesh_polyfill};
        for (auto const& g : _groups)
            result.groups.push_back(g);
        result.static_samplers.push_back_range(_static_samplers);
        return result;
    }

protected:
    /// `structural_hash` must come from sg::impl::pipeline_layout_hash over the creation arguments.
    pipeline_layout(cc::hash128 structural_hash,
                    cc::span<binding_group_layout_handle const> groups,
                    cc::optional<binding> inline_constants,
                    bool dispatch_grid = false,
                    cc::span<bound_sampler const> static_samplers = {},
                    mesh_polyfill_slots mesh_polyfill = mesh_polyfill_slots::none)
      : _structural_hash(structural_hash),
        _inline_constants(cc::move(inline_constants)),
        _has_dispatch_grid(dispatch_grid),
        _mesh_polyfill(mesh_polyfill)
    {
        _static_samplers.push_back_range(static_samplers);
        for (auto const& group : groups)
            _groups.push_back(group);
    }

    cc::hash128 _structural_hash;

private:
    cc::small_vector<binding_group_layout_handle, max_binding_groups> _groups;
    cc::optional<binding> _inline_constants;
    bool _has_dispatch_grid = false;
    mesh_polyfill_slots _mesh_polyfill = mesh_polyfill_slots::none;
    cc::vector<bound_sampler> _static_samplers;
};
