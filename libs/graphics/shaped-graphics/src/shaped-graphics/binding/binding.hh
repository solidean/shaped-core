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
/// It names the resource, as SGL does; what the shader does with it is the binding's separate `access`.
/// A kind and an access together map 1:1 to a view's (view_class, view_shape); see view_class_of / shape_of.
enum class sg::binding_type
{
    uniform_buffer,         ///< uniform block    — CBV / UBO
    buffer,                 ///< array of T       — structured SRV / UAV, SSBO
    bytes,                  ///< raw bytes        — byte-addressed SRV / UAV
    texture,                ///< sampled texture  — SRV
    image,                  ///< storage image    — UAV, whatever its access
    sampler,                ///< texture sampler  — not a view; bound as a static or dynamic sampler
    acceleration_structure, ///< ray-tracing TLAS — SRV addressed by GPU VA (HLSL RaytracingAccelerationStructure)
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

/// What a binding lets the shader do with its resource — SGL's unmarked, `out` and `mut`.
/// Only an image takes all three; a buffer or bytes is `read` or `read_write`, and every other kind is `read`.
/// Core WebGPU allows a `read_write` image only in r32float, r32uint and r32sint, so an image of any other format is `write` or `read` there.
/// dx12 and vulkan build the same UAV for all three, so to them it decides only the hazards.
enum class sg::access_mode
{
    read,       ///< the shader only loads
    write,      ///< the shader only stores — an image alone
    read_write, ///< both, which HLSL's RW types always declare
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
/// A sampler binding carries no view — no view class, no layout — so it is matched to a `sampler`, not a `raw_view`.
[[nodiscard]] constexpr bool is_sampler(binding_type t)
{
    return t == binding_type::sampler;
}

/// Whether `access` is one a binding of kind `t` may carry.
/// Only an image is ever write-only, since no target has a write-only buffer; only a buffer, bytes or an image writes at all.
[[nodiscard]] constexpr bool is_valid_access(binding_type t, access_mode access)
{
    switch (t)
    {
    case binding_type::image:
        return true;
    case binding_type::buffer:
    case binding_type::bytes:
        return access != access_mode::write;
    default:
        return access == access_mode::read;
    }
}

/// The view class a bound view must have to satisfy a binding of kind `t` with `access`.
/// A buffer's access picks its class; an image is one class whatever its access, since its view carries none.
[[nodiscard]] constexpr view_class view_class_of(binding_type t, access_mode access)
{
    switch (t)
    {
    case binding_type::uniform_buffer:
        return view_class::uniform;
    case binding_type::buffer:
    case binding_type::bytes:
        return access == access_mode::read ? view_class::readonly : view_class::readwrite;
    case binding_type::texture:
        return view_class::texture;
    case binding_type::image:
        return view_class::image;
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
    case binding_type::buffer:
        return view_shape::structured;
    case binding_type::bytes:
        return view_shape::bytes;
    case binding_type::texture:
    case binding_type::image:
        return view_shape::texture;
    case binding_type::acceleration_structure:
        return view_shape::acceleration_structure;
    case binding_type::sampler:
        break; // a sampler is not a view — callers gate on is_sampler() first
    }
    return view_shape::uniform_block; // unreachable for the view kinds above
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

    /// The name the shader's compiler reflected, where a compiler edge renamed it to `name`; empty otherwise.
    /// For diagnostics only: no backend and no layout reads it, and a cached shader never carries it.
    cc::string reflected_name;

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

    /// What the shader does with the resource; must be `is_valid_access(type, access)`.
    /// For an image it reaches WebGPU's layout and the hazards; for a buffer or bytes it also picks SRV or UAV.
    access_mode access = access_mode::read;

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

    /// For `image` bindings: the texel format the shader declared (`image2d[.rgba8_unorm]`).
    /// A WebGPU storage-texture layout entry requires it, and a layout is built before any view exists — so it
    /// cannot be taken from the bound view the way dx12 and vulkan take it.
    cc::optional<pixel_format> image_format;

    /// For `texture` bindings: how the texels are read.
    /// See texture_sample_type.
    cc::optional<texture_sample_type> sample_type;

    /// For `sampler` bindings: which kind of sampler.
    /// See sampler_binding_type.
    cc::optional<sampler_binding_type> sampler_type;

    /// Whether this is an array binding (count > 1): one descriptor per element, vacant elements as
    /// `sg::vacant_view`, and access declared explicitly per dispatch rather than inferred.
    [[nodiscard]] constexpr bool is_array() const { return count > 1; }

    /// Whether the shader may write the resource, which WebGPU forbids in the vertex stage.
    [[nodiscard]] constexpr bool is_writable() const { return access != access_mode::read; }
};

namespace sg
{

/// The view class a bound view must have to satisfy `b`.
[[nodiscard]] constexpr view_class view_class_of(binding const& b)
{
    return view_class_of(b.type, b.access);
}

/// Whether `a` and `b` are the same kind of binding, so one descriptor can serve both.
/// A buffer's or bytes' access is part of that, since it picks SRV or UAV; an image's is not, since one UAV serves all three.
[[nodiscard]] constexpr bool is_same_kind(binding const& a, binding const& b)
{
    return a.type == b.type && (a.type == binding_type::image || a.access == b.access);
}

/// Whether a bound view satisfies binding `b` — its view class and layout must match.
/// An image binding that declares its `image_format` also needs the view in exactly that format, which WebGPU and vulkan require.
/// A view of format `undefined` reads as its texture's own format.
/// The vacant marker satisfies every view kind: what a null descriptor looks like is the binding's to say,
/// and whether a vacancy is *allowed* there (array elements only) is the group creation's check, not this one.
[[nodiscard]] inline bool accepts(binding const& b, raw_view const& v)
{
    if (is_sampler(b.type))
        return false; // samplers are bound as samplers, never as views
    if (is_vacant(v))
        return true;
    if (view_class_of(v) != view_class_of(b) || shape_of(v) != shape_of(b.type))
        return false;
    if (b.type != binding_type::image || !b.image_format.has_value())
        return true;
    auto const& t = as_texture_view(v);
    auto const format = t.format != pixel_format::undefined || t.texture == nullptr ? t.format : t.texture->format();
    return format == b.image_format.value();
}

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
