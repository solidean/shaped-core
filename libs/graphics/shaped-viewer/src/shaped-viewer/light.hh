#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// Light types a view holds.
///
/// A view keeps one typed list per kind (view::area_lights, and more as they land), rather than a single tagged list.
/// So a consumer iterates exactly the type it handles, without switching on a tag or partitioning by kind.
/// This is deliberately explicit, close-to-the-renderer API.
/// The ergonomic scene-building layer sits on top of it.

/// A rectangular area light the path tracer samples for direct lighting (next-event estimation).
/// One shadow ray per bounce is aimed at the rect, so it lights the scene even when no emissive geometry is in view.
///
/// The rect is given directly in world space: a center plus the two half-extent vectors spanning it, so a point on it is `center + s * half_extent_u + t * half_extent_v` for s, t in [-1, 1].
/// The emitting face is the one `cross(half_extent_u, half_extent_v)` points along — swap the two half-extents to flip it.
/// The two must not be parallel: a rect collapsed to a line or a point has no normal.
struct area_light
{
    tg::pos3f center;
    tg::vec3f half_extent_u;
    tg::vec3f half_extent_v;

    /// emitted radiance per channel; every component must be >= 0.
    /// The default is negative — no light emits that — so a light that never got an emission is reported when it is used instead of tracing as a black rect.
    tg::vec3f emission = tg::vec3f(-1.0f);
};

/// GPU-side rectangular area light, matching the `AreaLight` struct in shaders/light.hlsli.
///
/// The rect the integrator samples: a center plus the two half-edge vectors spanning it, so a sample is `center + s * u + t * v` for s, t in [-1, 1].
/// Each `tg::vec3f` sits in its own 16-byte lane (the trailing pad scalars) — the std140-ish cbuffer layout HLSL expects, so this uploads straight into a uniform buffer.
///
/// A tighter packing is available when the block starts to matter (many lights per view, or the cbuffer running out of room):
/// `normal` is redundant — the integrator can form `normalize(cross(u, v))` itself — and the five pad scalars are free lanes the remaining fields could fold into, taking 80 bytes to 64 or less.
/// It stays spelled out for now: one lane per vector reads directly against the shader struct.
struct area_light_gpu
{
    tg::vec3f center;
    f32 _pad0 = 0;
    tg::vec3f u; // world half-extent spanning the rect's first axis
    f32 _pad1 = 0;
    tg::vec3f v; // world half-extent spanning the rect's second axis
    f32 _pad2 = 0;
    tg::vec3f emission;
    f32 _pad3 = 0;
    tg::vec3f normal; // = normalize(cross(u, v)); the emitting face
    f32 _pad4 = 0;

    /// Lays the light's rect out in the GPU lanes, `normal` formed from its two half-extents.
    /// `light.half_extent_u` / `half_extent_v` must not be parallel — the normal is undefined for a degenerate rect.
    /// A negative `emission` component means the light never got one: it passes through unchanged, reported once to stderr.
    [[nodiscard]] static area_light_gpu from(area_light const& light);
};
} // namespace sv
