#pragma once

// One light as the path tracer reads it.
//
// Its own file because the binding group that declares the light buffer (pt_common.hlsli) and the estimators that read
// it both need the type, and neither should own it.

namespace sv
{
// The path a light takes through the integrator — mirrors sv::light_path (scene/light.hh), whose values these are.
static const uint light_path_point = 0;
static const uint light_path_area = 1;
static const uint light_path_distant_point = 2;
static const uint light_path_distant_disc = 3;

// Bits of `light::flags` — mirrors sv::light_gpu::flag_*; 0 is the default for each.
static const uint light_flag_two_sided = 1u << 0;
static const uint light_flag_visible_to_camera = 1u << 1;
static const uint light_flag_casts_no_shadow = 1u << 2;

/// One light, tagged by `path` — mirrors sv::light_gpu (scene/light.hh).
///
/// Keep the two in lockstep: this is a byte layout, not a description of one.
/// Units are resolved on the CPU, so `emission` is one canonical quantity per path: a point's intensity, an area's
/// radiance per face, a distant point's irradiance, a disc's radiance.
struct light
{
    float3 position; // point: the source; area: the rect's center
    uint path;       // a light_path_* value
    float3 u;        // area: world half-extent spanning the rect's first axis
    float area;      // area: the world area of one face
    float3 v;        // area: world half-extent spanning the rect's second axis
    uint flags;      // light_flag_* bits
    float3 emission; // the canonical quantity of the path
    float cone_scale;
    float3 normal;   // the light's -Z: a rect's front face, a spot's axis, the direction a distant light travels
    float cone_offset;
    float cos_angular_radius; // distant_disc: cosine of the disc's angular radius
    uint link_mask;           // RESERVED for light linking, read by nothing yet
    float2 _pad0;
};

/// Whether geometry between `l` and a surface blocks it.
bool light_casts_shadows(light l)
{
    return (l.flags & light_flag_casts_no_shadow) == 0u;
}

/// Whether a camera ray reaching `l` sees it.
bool light_visible_to_camera(light l)
{
    return (l.flags & light_flag_visible_to_camera) != 0u;
}

/// How much of a light survives its cone along a direction making cosine `cos_axis` with the light's axis.
///
/// glTF's falloff, `saturate(cos * scale + offset)^2`, which an unshaped light turns into a constant 1.
/// Both estimators call this, so the density they share and the emission they weight agree on what the cone removed.
float light_cone(light l, float cos_axis)
{
    float a = saturate(cos_axis * l.cone_scale + l.cone_offset);
    return a * a;
}
} // namespace sv
