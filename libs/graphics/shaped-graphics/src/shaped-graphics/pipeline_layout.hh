#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/sampler.hh>

namespace sg
{
/// A static sampler bound directly to a shader register, for attaching to a pipeline_layout — the
/// register-bound counterpart to a group layout's name-matched `named_sampler`. Use this for a static
/// sampler a pipeline needs on top of (or independent of) its group layouts. `binding` carries the
/// register/space (set/index) and count; its `type` must be a sampler binding.
struct bound_sampler
{
    sg::binding binding;
    sg::sampler sampler;
};

/// Description for building a pipeline_layout: an ordered list of binding_group_layouts (one per bind slot)
/// plus any extra register-bound static samplers. `groups[i]` is the schema bound at slot `i` (see
/// cmd.compute.bind_group's `set` index).
struct pipeline_layout_description
{
    // Ordered; index = bind slot. Owning + inline-capped at max_binding_groups (no heap for the common case).
    // TODO: cc::fixed_vector<binding_group_layout_handle, max_binding_groups> is the better match once it lands.
    cc::small_vector<binding_group_layout_handle, max_binding_groups> groups;

    // Extra static samplers bound directly to shader registers, not tied to any group's bindings — baked
    // into the root signature alongside the group layouts' own (name-matched) static samplers.
    cc::vector<bound_sampler> static_samplers;

    /// An optional constant buffer binding that provides inline (root/push) constants, allowing fast
    /// per-draw parameter updates without allocating separate descriptor space. Set directly via
    /// `cmd.compute.set_inline_constants(...)` and similar. Maps to dx12 root constants / vulkan push
    /// constants.
    ///
    /// This binding must be a constant buffer (no other type permitted) and must be excluded from the
    /// group layouts to avoid allocation conflicts. `block_size` must be set and a multiple of 4.
    ///
    /// Sizing guidance:
    /// - 0–64 bytes (16 floats / 1 mat4): always preferable to separate buffers.
    /// - Up to 128 bytes (32 floats / 2 mat4): acceptable but avoid changing every draw; reserve for
    ///   pass-level data or other parameters that update infrequently.
    /// - Beyond 128 bytes: usually a mistake, use inline-uploaded buffers instead.
    /// Reserve inline constants for small data that genuinely varies per invocation.
    cc::optional<binding> inline_constants;
};

/// The binding interface a pipeline is compiled against: an ordered set of binding_group_layouts (+, in
/// the future, root constants). Composing groups into slots lets an entire group be rebound at one slot
/// without disturbing the others. Held via pipeline_layout_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 root signature, vulkan
/// VkPipelineLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class pipeline_layout
{
public:
    virtual ~pipeline_layout();

protected:
    pipeline_layout() = default;
};
} // namespace sg
