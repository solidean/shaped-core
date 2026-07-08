#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/views.hh>

/// A shader's reflected resource **bindings** — what a compiled_shader declares it needs at each slot,
/// backend-agnostic. A binding is matched by name to a bound view when a binding_group is built. See
/// libs/graphics/shaped-graphics/docs/concepts/bindings.md.

namespace sg
{
/// The kind of resource a shader binding expects — the backend-agnostic reflection vocabulary (the
/// portable stand-in for HLSL's D3D_SHADER_INPUT_TYPE). Buffer kinds map 1:1 to a view's
/// (view_class, view_shape); see access_of / shape_of.
enum class binding_type
{
    uniform_buffer,              ///< uniform block   — CBV / UBO
    readonly_structured_buffer,  ///< read array of T — SRV structured / read SSBO
    readwrite_structured_buffer, ///< rw array of T   — UAV structured / rw SSBO
    readonly_raw_buffer,         ///< read raw bytes  — SRV byte-addressed
    readwrite_raw_buffer,        ///< rw raw bytes    — UAV byte-addressed
    sampled_texture,             ///< sampled texture — SRV (readonly, shape texture)
    storage_texture,             ///< storage texture — UAV (readwrite, shape texture)
    // Future (with a graphics pipeline / samplers): sampler, acceleration_structure.
};

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
    case binding_type::sampled_texture:
        return view_class::readonly;
    case binding_type::storage_texture:
        return view_class::readwrite;
    }
    return view_class::uniform; // unreachable for the closed set above
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
    case binding_type::sampled_texture:
    case binding_type::storage_texture:
        return view_shape::texture;
    }
    return view_shape::uniform_block; // unreachable for the closed set above
}

/// Whether a bound view satisfies a binding of this type — its access and layout must match.
[[nodiscard]] inline bool accepts(binding_type t, raw_view const& v)
{
    return v.access == access_of(t) && v.shape == shape_of(t);
}

/// A single reflected shader resource binding: a named slot the shader reads. Identified by a
/// backend-agnostic (set, index) address — SPIR-V set/binding, WGSL @group/@binding, Metal argument
/// index; a D3D12 backend derives (register-type from `type`, register = index, space = set).
struct binding
{
    cc::string name; ///< reflection name — the key a binding_group matches a bound view against
    u32 set = 0;     ///< descriptor set / @group
    u32 index = 0;   ///< binding within the set / @binding
    u32 count = 1;   ///< array length; 0 = unbounded array
    binding_type type = binding_type::uniform_buffer;

    /// For `uniform_buffer` bindings: the declared block size in bytes, used to validate a bound
    /// view's size. Absent for other kinds.
    cc::optional<isize> block_size;
};
} // namespace sg
