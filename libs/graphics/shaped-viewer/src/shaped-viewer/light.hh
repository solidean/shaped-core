#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// Light types a view holds.
///
/// A view keeps one typed list per kind (view::area_lights, and more as they land), rather than a single tagged list — so a consumer iterates exactly the type it handles without switching on a tag or partitioning by kind.
/// This is deliberately explicit, close-to-the-renderer API.
/// The ergonomic scene-building layer sits on top of it.

/// A rectangular area light the path tracer samples for direct lighting (next-event estimation).
/// One shadow ray per bounce is aimed at the rect, so it lights the scene even when no emissive geometry is in view.
///
/// The rectangle is defined once in a canonical local frame and placed by `transform`: it lies in the local xy plane centered at the origin, half-sized by `half_extents` along local x and y, and its emitting face looks along local +z.
/// `transform` positions and orients it in the world (rotation, scale and translation all apply).
/// The view_renderer derives the world-space rect the integrator needs — center, two half-edge vectors, and the outward normal — from this.
///
/// The default is a downward-facing rect three units overhead, a sensible key light for a scene near the origin.
struct area_light
{
    tg::vec2f half_extents = tg::vec2f(0.75f, 0.75f);

    // Columns are the local basis images + translation: local x -> world x, local y -> world +z, local z (the emitting normal) -> world -y (down), translated three units up.
    tg::mat4f transform = tg::mat4f::make_from_cols(tg::vec4f(1, 0, 0, 0),
                                                    tg::vec4f(0, 0, 1, 0),
                                                    tg::vec4f(0, -1, 0, 0),
                                                    tg::vec4f(0, 3, 0, 1));

    tg::vec3f emission = tg::vec3f(12.0f, 12.0f, 12.0f);
};

/// GPU-side rectangular area light, matching the `AreaLight` struct in shaders/light.hlsli.
///
/// The rect the integrator samples, resolved into world space: a center plus the two half-edge vectors spanning it, so a sample is `center + s * u + t * v` for s, t in [-1, 1].
/// Each `tg::vec3f` sits in its own 16-byte lane (the trailing pad scalars) — the std140-ish cbuffer layout HLSL expects, so this uploads straight into a uniform buffer.
///
/// The default is the GPU image of a default-constructed `area_light`.
///
/// A tighter packing is available when the block starts to matter (many lights per view, or the cbuffer running out of room):
/// `normal` is redundant — the integrator can form `normalize(cross(u, v))` itself — and the five pad scalars are free lanes the remaining fields could fold into, taking 80 bytes to 64 or less.
/// It stays spelled out for now: one lane per vector reads directly against the shader struct.
struct area_light_gpu
{
    tg::vec3f center = tg::vec3f(0, 3, 0);
    f32 _pad0 = 0;
    tg::vec3f u = tg::vec3f(0.75f, 0, 0); // world half-edge along the rect's local x
    f32 _pad1 = 0;
    tg::vec3f v = tg::vec3f(0, 0, 0.75f); // world half-edge along the rect's local y
    f32 _pad2 = 0;
    tg::vec3f emission = tg::vec3f(12.0f, 12.0f, 12.0f);
    f32 _pad3 = 0;
    tg::vec3f normal = tg::vec3f(0, -1, 0); // = normalize(cross(u, v)); the emitting face
    f32 _pad4 = 0;

    /// Resolves the rectangle-plus-transform into the world-space rect: `u` / `v` are the world images of the local x / y half-extents, and `normal` follows from them, staying correct under rotation and non-uniform scale.
    /// `light.transform` must not collapse the rect's plane — the normal is undefined for a degenerate rect.
    [[nodiscard]] static area_light_gpu from(area_light const& light);
};
} // namespace sv
