#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// A shader's reflected resource **bindings** — what a compiled_shader declares it needs at each slot, backend-agnostic.
/// A binding is matched by name to a bound view when a binding_group is built.
/// See libs/graphics/shaped-graphics/docs/concepts/bindings.md.

/// The kind of resource a shader binding expects — the backend-agnostic reflection vocabulary, the portable stand-in for HLSL's D3D_SHADER_INPUT_TYPE.
/// Buffer kinds map 1:1 to a view's (view_class, view_shape); see access_of / shape_of.
enum class sg::binding_type
{
    uniform_buffer,              ///< uniform block   — CBV / UBO
    readonly_structured_buffer,  ///< read array of T — SRV structured / read SSBO
    readwrite_structured_buffer, ///< rw array of T   — UAV structured / rw SSBO
    readonly_raw_buffer,         ///< read raw bytes  — SRV byte-addressed
    readwrite_raw_buffer,        ///< rw raw bytes    — UAV byte-addressed
    readonly_texture,            ///< sampled texture — SRV (readonly, shape texture)
    readwrite_texture,           ///< storage texture — UAV (readwrite, shape texture)
    sampler,                     ///< texture sampler — not a view; bound as a static or dynamic sampler
    acceleration_structure,      ///< ray-tracing TLAS — SRV addressed by GPU VA (HLSL RaytracingAccelerationStructure)
};

namespace sg
{

/// Whether a binding is a sampler rather than a resource view.
/// A sampler binding carries no view — no access class, no layout — so it is matched to a `sampler`, not a `raw_view`.
[[nodiscard]] constexpr bool is_sampler(binding_type t)
{
    return t == binding_type::sampler;
}

/// The access class a bound view must have to satisfy a binding of this type.
[[nodiscard]] constexpr view_class access_of(binding_type t)
{
    switch (t)
    {
    case binding_type::uniform_buffer:
        return view_class::uniform;
    case binding_type::readonly_structured_buffer:
    case binding_type::readonly_raw_buffer:
        return view_class::readonly;
    case binding_type::readwrite_structured_buffer:
    case binding_type::readwrite_raw_buffer:
        return view_class::readwrite;
    case binding_type::readonly_texture:
        return view_class::readonly;
    case binding_type::readwrite_texture:
        return view_class::readwrite;
    case binding_type::acceleration_structure:
        return view_class::acceleration_structure;
    case binding_type::sampler:
        break; // a sampler is not a view — callers gate on is_sampler() first
    }
    return view_class::uniform; // unreachable for the view kinds above
}

/// The layout a bound view must have to satisfy a binding of this type.
[[nodiscard]] constexpr view_shape shape_of(binding_type t)
{
    switch (t)
    {
    case binding_type::uniform_buffer:
        return view_shape::uniform_block;
    case binding_type::readonly_structured_buffer:
    case binding_type::readwrite_structured_buffer:
        return view_shape::structured;
    case binding_type::readonly_raw_buffer:
    case binding_type::readwrite_raw_buffer:
        return view_shape::raw;
    case binding_type::readonly_texture:
    case binding_type::readwrite_texture:
        return view_shape::texture;
    case binding_type::acceleration_structure:
        return view_shape::acceleration_structure;
    case binding_type::sampler:
        break; // a sampler is not a view — callers gate on is_sampler() first
    }
    return view_shape::uniform_block; // unreachable for the view kinds above
}

/// Whether a bound view satisfies a binding of this type — its access and layout must match.
/// The vacant marker satisfies every view kind: what a null descriptor looks like is the binding's to say,
/// and whether a vacancy is *allowed* there (array elements only) is the group creation's check, not this one.
[[nodiscard]] inline bool accepts(binding_type t, raw_view const& v)
{
    if (is_sampler(t))
        return false; // samplers are bound as samplers, never as views
    if (is_vacant(v))
        return true;
    return access_of(v) == access_of(t) && shape_of(v) == shape_of(t);
}

} // namespace sg

/// A single reflected shader resource binding: a named slot the shader reads.
/// Identified by a backend-agnostic (set, index) address — SPIR-V set/binding, WGSL @group/@binding, Metal argument index.
/// A D3D12 backend derives (register-type from `type`, register = index, space = set).
struct sg::binding
{
    cc::string name; ///< reflection name — the key a binding_group matches a bound view against
    u32 set = 0;     ///< descriptor set / @group
    u32 index = 0;   ///< binding within the set / @binding
    u32 count = 1;   ///< array length; 0 = unbounded array
    binding_type type = binding_type::uniform_buffer;

    /// For `uniform_buffer` bindings: the declared block size in bytes, used to validate a bound view's size.
    /// Absent for other kinds.
    cc::optional<isize> block_size;

    /// For texture bindings: the shader-declared view dimension (`Texture2D` vs `TextureCube` vs ...); absent for other kinds.
    /// Reflection fills it; hand-written array bindings must set it — it is what lets a backend synthesize a
    /// dimension-correct null descriptor for a vacant element.
    cc::optional<texture_view_dimension> texture_dimension;

    /// Whether this is an array binding (count > 1): one descriptor per element, vacant elements as
    /// `sg::vacant_view`, and access declared explicitly per dispatch rather than inferred.
    [[nodiscard]] constexpr bool is_array() const { return count > 1; }
};

namespace sg
{

/// Appends every binding of `from` whose name `into` does not already carry.
/// One pipeline has one binding interface, so a multi-stage pipeline's group layout must cover the union of
/// its stages' reflected bindings — merge them stage by stage, then hand the result to a group layout.
/// A name already in `into` keeps its existing entry: two stages disagreeing on (set, index), count or type
/// is a shader bug this does not detect.
void merge_bindings(cc::vector<binding>& into, cc::span<binding const> from);

/// The union of all `stages`' bindings by name, in first-seen order — the merge above over several stages.
[[nodiscard]] cc::vector<binding> merge_bindings(cc::span<cc::span<binding const> const> stages);

/// Removes the sampler bindings from `bindings` and returns them, both keeping their relative order.
/// Split them off for the samplers bound *outside* the group — a register-bound `bound_sampler` on the
/// pipeline_layout needs no group binding, and leaving one in the group layout claims the same register a
/// second time (as a dynamic sampler), which the backend rejects.
/// Sampler bindings kept in `bindings` are the ones the group layout binds: name-matched static, or dynamic per group.
[[nodiscard]] cc::vector<binding> split_off_sampler_bindings(cc::vector<binding>& bindings);
} // namespace sg
