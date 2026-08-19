#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// GPU layout of one PBR material, mirroring the `PbrMaterial` struct the shading closest-hit reads out of a StructuredBuffer.
/// The field order and the two trailing scalars are chosen so each `float3` sits in its own 16-byte lane with no implicit padding — keep this in lockstep with shaders/pbr.hlsli.
struct sv::pbr_material_gpu
{
    tg::vec3f base_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    f32 metallic = 0.0f;
    tg::vec3f emissive = tg::vec3f(0.0f, 0.0f, 0.0f);
    f32 roughness = 0.5f;

    /// Packs a scene-side material into its GPU lane layout.
    [[nodiscard]] static pbr_material_gpu from(pbr_material const& m);
};

/// A basic metallic-roughness PBR material.
/// Flat per-primitive for now: the closest-hit shader indexes one of these per triangle by `PrimitiveIndex()`.
/// A single mesh can therefore carry a different material on every triangle without any texture lookups.
struct sv::pbr_material
{
    tg::vec3f base_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    tg::vec3f emissive = tg::vec3f(0.0f, 0.0f, 0.0f);
};

/// The attribute names per-face PBR travels under.
///
/// A material *definition* should be what declares which attributes it samples; none exists yet, so these four names
/// are the contract between whoever fills a mesh and the repack that feeds the trace (see libs/graphics/shaped-viewer/docs/TODO.md).
/// Each is `per_triangle`, and an attribute a mesh does not carry falls back to `pbr_material`'s default for that
/// field rather than failing.
namespace sv::pbr_attribute
{
inline constexpr cc::string_view base_color = "base_color"; ///< tg::vec3f
inline constexpr cc::string_view metallic = "metallic";     ///< f32
inline constexpr cc::string_view roughness = "roughness";   ///< f32
inline constexpr cc::string_view emissive = "emissive";     ///< tg::vec3f
} // namespace sv::pbr_attribute

namespace sv
{
/// Scalarizes an array-of-structs material range into the four `per_triangle` attributes named above.
///
/// TEMPORARY: `pbr_material` is a struct, and `mesh_attribute` only carries scalars and tg vectors / matrices, so one
/// material array cannot yet be one attribute.
/// This whole function goes away once `mesh_attribute::create` accepts a struct — the four names then collapse back
/// into a single attribute the caller creates directly.
[[nodiscard]] cc::vector<mesh_attribute> pbr_material_attributes(cc::span<pbr_material const> materials);

inline pbr_material_gpu pbr_material_gpu::from(pbr_material const& m)
{
    return {.base_color = m.base_color, .metallic = m.metallic, .emissive = m.emissive, .roughness = m.roughness};
}
} // namespace sv
