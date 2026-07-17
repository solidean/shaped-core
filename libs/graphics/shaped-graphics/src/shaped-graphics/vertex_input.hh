#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// The vertex-input layout of a raster pipeline: how the bytes of each bound vertex buffer decode into
/// the shader's per-vertex inputs. Two ways to build one: fill the struct explicitly, or derive it from
/// vertex struct types with `vertex_input_layout::create<Vs...>()` (see vertex_layout_of below).

namespace sg
{
/// The component type + count of a single vertex attribute. Deliberately small; grow as needed. Each
/// enumerator maps 1:1 to a DXGI / Vulkan vertex format.
enum class vertex_attribute_format
{
    f32,   // DX12 R32_FLOAT          / Vk R32_SFLOAT
    vec2f, // DX12 R32G32_FLOAT       / Vk R32G32_SFLOAT
    vec3f, // DX12 R32G32B32_FLOAT    / Vk R32G32B32_SFLOAT
    vec4f, // DX12 R32G32B32A32_FLOAT / Vk R32G32B32A32_SFLOAT

    i32,   // DX12 R32_SINT           / Vk R32_SINT
    vec2i, // DX12 R32G32_SINT        / Vk R32G32_SINT
    vec3i, // DX12 R32G32B32_SINT     / Vk R32G32B32_SINT
    vec4i, // DX12 R32G32B32A32_SINT  / Vk R32G32B32A32_SINT

    u32,   // DX12 R32_UINT           / Vk R32_UINT
    vec2u, // DX12 R32G32_UINT        / Vk R32G32_UINT
    vec3u, // DX12 R32G32B32_UINT     / Vk R32G32B32_UINT
    vec4u, // DX12 R32G32B32A32_UINT  / Vk R32G32B32A32_UINT

    rgba8_unorm, // DX12 R8G8B8A8_UNORM / Vk R8G8B8A8_UNORM — packed color
    rgba8_uint,  // DX12 R8G8B8A8_UINT  / Vk R8G8B8A8_UINT
};

/// One attribute pulled from a vertex buffer: which shader input it feeds (`semantic` + `semantic_index`,
/// matched via reflection), its component format, its byte offset within the vertex, and which bound
/// buffer (`slot`) it comes from.
///
/// TODO: identify inputs by a backend-neutral numeric `location` instead of an HLSL `semantic` string
/// (the only cross-language identity — SPIR-V/WGSL/Metal all match by location), moving the semantic into
/// compiled_shader's reflected I/O signature so only the dx12 backend resolves location -> semantic. See
/// libs/graphics/shaped-graphics/docs/TODO.md.
struct vertex_attribute
{
    cc::string semantic;    ///< HLSL semantic (e.g. "POSITION") the input is matched by
    u32 semantic_index = 0; ///< semantic index (matrix rows / arrays)
    vertex_attribute_format format = vertex_attribute_format::vec3f;
    cc::isize offset = 0; ///< byte offset of this attribute within its vertex
    int slot = 0;         ///< index of the bound vertex buffer this attribute reads from
};

/// One bound vertex buffer: the stride between consecutive vertices and whether it advances per vertex
/// or per instance.
struct vertex_input_slot
{
    cc::isize stride = 0;      ///< bytes between consecutive elements in this buffer
    bool per_instance = false; ///< false: advance per vertex; true: advance per instance
};

/// The complete vertex-input layout: one `slots` entry per bound vertex buffer, and the flat list of
/// `attributes` (each naming its slot). Fill it directly, or build it from vertex types via `create`.
struct vertex_input_layout
{
    cc::small_vector<vertex_input_slot, 8> slots;
    cc::vector<vertex_attribute> attributes;

    /// Build a layout from vertex struct types: one slot per `Vs...` (slot index = argument position),
    /// its stride/attributes taken from `vertex_layout_of<V>`. Specialize `vertex_layout_of` for each
    /// vertex struct. Attributes' `slot` is filled in here from the pack position.
    template <class... Vs>
    [[nodiscard]] static vertex_input_layout create();
};

/// One vertex type's contribution to a layout: its stride, its attributes (with `slot` unset — `create`
/// assigns it), and whether it is a per-instance buffer. The value a `vertex_layout_of<V>` returns.
struct vertex_type_layout
{
    cc::isize stride = 0;
    bool per_instance = false;
    cc::vector<vertex_attribute> attributes;
};

/// Customization point for `vertex_input_layout::create<V>()`: specialize for a vertex struct `V` with a
/// `static vertex_type_layout get()`. Example:
///
///     template <> struct sg::vertex_layout_of<my_vertex> {
///         static sg::vertex_type_layout get() {
///             return {.stride = sizeof(my_vertex),
///                     .attributes = {{.semantic = "POSITION", .format = vertex_attribute_format::vec3f,
///                                     .offset = offsetof(my_vertex, position)}, ...}};
///         }
///     };
template <class V>
struct vertex_layout_of;

template <class... Vs>
vertex_input_layout vertex_input_layout::create()
{
    vertex_input_layout layout;
    int slot = 0;
    (
        [&]
        {
            vertex_type_layout vt = vertex_layout_of<Vs>::get();
            layout.slots.push_back({.stride = vt.stride, .per_instance = vt.per_instance});
            for (auto& attr : vt.attributes)
            {
                attr.slot = slot;
                layout.attributes.push_back(cc::move(attr));
            }
            ++slot;
        }(),
        ...);
    return layout;
}
} // namespace sg
