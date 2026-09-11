#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/shader_stage.hh>
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

/// How a sampled texture binding's texels are read, which decides whether a sampler may filter them.
/// WebGPU requires it on a bind group layout entry and rejects a mismatch outright: a 32-bit float texture is
/// unfilterable there, so a layout claiming `filterable` over one fails validation rather than running slowly.
/// dx12 and vulkan do not ask, so an absent value costs them nothing.
enum class sg::texture_sample_type
{
    filterable_float,   ///< the ordinary case: unorm / snorm / 16-bit float, which a linear sampler may filter
    unfilterable_float, ///< 32-bit float channels — sampled as float, never filtered
    depth,              ///< a depth texture, read through a comparison or an ordinary sampler
    sint,               ///< signed integer texels, never filtered
    uint,               ///< unsigned integer texels, never filtered
};

/// What kind of sampler a sampler binding expects.
/// WebGPU requires it on the layout, before any sampler is bound, so it cannot be derived from the bound `sg::sampler`.
enum class sg::sampler_binding_type
{
    filtering,     ///< may use a linear min/mag/mip filter
    non_filtering, ///< nearest only — the only kind an unfilterable-float texture accepts
    comparison,    ///< a shadow sampler, carrying a compare_op
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
/// `index` is the address within its group — SPIR-V @binding, WGSL @binding, HLSL register number, Metal argument index.
/// `group_index` and `space` are the two ways a shading language namespaces that address, and a binding carries whichever its language reflects.
///
/// The two are not interchangeable, which is why neither one stands in for the other:
/// a **group index** is a hardware-visible descriptor set that the bind slot must match (SPIR-V `set`, WGSL `@group`),
/// while a **space** is only a namespace for register numbers and never reaches the descriptor table (HLSL `space`).
struct sg::binding
{
    cc::string name; ///< reflection name — the key a binding_group matches a bound view against

    /// The descriptor set / @group this binding lives in — reflected by SPIR-V and other languages where the set is part of the hardware binding.
    /// Present means the whole chain is pinned: the group layout inherits it, and binding a group at any other slot is an error.
    /// Absent means the shading language does not fix a group, and the bind slot alone decides.
    cc::optional<u32> group_index;

    /// The HLSL register space this binding's `index` is numbered in — reflected by DXC, absent elsewhere.
    /// A namespace for register numbers only: it never constrains which slot the group is bound at.
    /// Absent means the shading language has no register spaces at all, which is NOT the same as space 0 —
    /// the structural layout hash keeps them apart, so dx12 requires an explicit one and asserts on absence.
    cc::optional<u32> space;

    u32 index = 0; ///< binding within the group / @binding / HLSL register number
    u32 count = 1; ///< array length; 0 = unbounded array
    binding_type type = binding_type::uniform_buffer;

    /// For `uniform_buffer` bindings: the declared block size in bytes, used to validate a bound view's size.
    /// Absent for other kinds.
    cc::optional<isize> block_size;

    /// For texture bindings: the shader-declared view dimension (`Texture2D` vs `TextureCube` vs ...); absent for other kinds.
    /// Reflection fills it; hand-written array bindings must set it — it is what lets a backend synthesize a
    /// dimension-correct null descriptor for a vacant element.
    cc::optional<texture_view_dimension> texture_dimension;

    /// The stages that declared this binding.
    /// A compiled_shader is one stage, so reflection sets exactly that bit and `merge_bindings` unions them as the
    /// stages are folded into one layout.
    ///
    /// **Empty means not known, not "no stage".** A hand-written binding that never says is treated as visible
    /// everywhere, which is what dx12 and vulkan did unconditionally before this field existed.
    /// It matters because WebGPU cannot be permissive here: its default limits allow zero storage buffers in the
    /// vertex stage, so a storage binding wrongly marked vertex-visible fails validation on a conformant device.
    shader_stages visibility;

    /// For `readwrite_texture` bindings: the texel format the shader declared (`RWTexture2D<float4>`).
    /// A WebGPU storage-texture layout entry requires it, and a layout is built before any view exists — so it
    /// cannot be taken from the bound view the way dx12 and vulkan take it.
    cc::optional<pixel_format> storage_format;

    /// For `readonly_texture` bindings: how the texels are read.
    /// See texture_sample_type.
    cc::optional<texture_sample_type> sample_type;

    /// For `sampler` bindings: which kind of sampler.
    /// See sampler_binding_type.
    cc::optional<sampler_binding_type> sampler_type;

    /// Whether this is an array binding (count > 1): one descriptor per element, vacant elements as
    /// `sg::vacant_view`, and access declared explicitly per dispatch rather than inferred.
    [[nodiscard]] constexpr bool is_array() const { return count > 1; }
};

namespace sg
{

/// Stamps `stage` into every binding's `visibility`.
///
/// A compiled_shader is exactly one stage, so this is what a compiler calls once its reflection is in hand — which
/// is why no reflector has to know which stage it was run for.
/// Additive rather than assigning, so calling it on bindings already merged across stages cannot narrow them.
void apply_stage_visibility(cc::span<binding> bindings, shader_stage stage);

/// Appends every binding of `from` whose name `into` does not already carry.
/// One pipeline has one binding interface, so a multi-stage pipeline's group layout must cover the union of
/// its stages' reflected bindings — merge them stage by stage, then hand the result to a group layout.
/// A name already in `into` keeps its existing entry, except for `visibility`, which is unioned: accumulating the
/// stages that declared a binding is the one thing this merge exists to do beyond deduplicating.
/// Two stages disagreeing on the address, count or type is a shader bug this does not detect.
void merge_bindings(cc::vector<binding>& into, cc::span<binding const> from);

/// The union of all `stages`' bindings by name, in first-seen order — the merge above over several stages.
[[nodiscard]] cc::vector<binding> merge_bindings(cc::span<cc::span<binding const> const> stages);

/// Removes the sampler bindings from `bindings` and returns them, both keeping their relative order.
/// Split them off for the samplers bound *outside* the group — a register-bound `bound_sampler` on the
/// pipeline_layout needs no group binding, and leaving one in the group layout claims the same register a
/// second time (as a dynamic sampler), which the backend rejects.
/// Sampler bindings kept in `bindings` are the ones the group layout binds: name-matched static, or dynamic per group.
[[nodiscard]] cc::vector<binding> split_off_sampler_bindings(cc::vector<binding>& bindings);

/// The one group index `bindings` declare, or nothing if none of them declares one.
/// Every binding that carries a `group_index` must carry the same one — they end up in one group layout, and a
/// group is bound at a single slot, so two of them naming different sets could not both be satisfied.
[[nodiscard]] cc::optional<u32> group_index_of(cc::span<binding const> bindings);
} // namespace sg
