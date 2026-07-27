#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// GPU layout of one PBR material, mirroring the `PbrMaterial` struct the shading closest-hit reads out of
/// a StructuredBuffer. The field order and the two trailing scalars are chosen so each `float3` sits in its
/// own 16-byte lane with no implicit padding — keep this in lockstep with shaders/pbr.hlsli.
struct pbr_material_gpu
{
    tg::vec3f base_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    f32 metallic = 0.0f;
    tg::vec3f emissive = tg::vec3f(0.0f, 0.0f, 0.0f);
    f32 roughness = 0.5f;

    /// Packs a scene-side material into its GPU lane layout.
    [[nodiscard]] static pbr_material_gpu from(pbr_material const& m);
};

/// A basic metallic-roughness PBR material. Flat per-primitive for now: the closest-hit shader indexes one
/// of these per triangle by `PrimitiveIndex()`, so a single mesh can carry a different material on every
/// triangle without any texture lookups.
struct pbr_material
{
    tg::vec3f base_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    tg::vec3f emissive = tg::vec3f(0.0f, 0.0f, 0.0f);
};

inline pbr_material_gpu pbr_material_gpu::from(pbr_material const& m)
{
    return {.base_color = m.base_color, .metallic = m.metallic, .emissive = m.emissive, .roughness = m.roughness};
}
} // namespace sv
