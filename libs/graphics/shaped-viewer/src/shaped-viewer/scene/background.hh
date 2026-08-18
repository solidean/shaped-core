#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The view's background / environment: the radiance a primary ray sees when it misses all geometry.
///
/// For now it is an order-3 RGB spherical-harmonics probe — 16 coefficients, each an RGB radiance, in the standard real-SH basis with index 0 the constant (DC) term.
/// Order 3 captures a sky gradient plus some directional structure (a soft sun disc, horizon banding), which is all a background needs.
/// A miss shader reconstructs the radiance along a ray direction from these coefficients.
///
/// All-zero is a black background.
/// The coefficients are raw SH, so writing one by hand means knowing the basis — the factories below are the way in.
/// They compose, because SH is linear: `gradient(...).combined_with(sun(...))` reconstructs the sky plus the sun.
struct sv::background
{
    static constexpr int sh_coefficient_count = 16; // order 3: (3 + 1)^2 real-SH coefficients

    tg::vec3f sh[sh_coefficient_count] = {};

    /// The same `radiance` in every direction — a flat ambient environment, band 0 alone.
    [[nodiscard]] static background uniform(tg::vec3f radiance);

    /// A vertical gradient: `zenith` looking straight up (+y), `nadir` straight down, and their average on the horizon.
    /// Exact rather than fitted — a radiance linear in the up component lives entirely in bands 0 and 1.
    [[nodiscard]] static background gradient(tg::vec3f zenith, tg::vec3f nadir);

    /// A soft directional lobe peaking at exactly `radiance` along `direction`, which must not be zero (it is normalized here) and points from the scene toward the light.
    ///
    /// The shape is the clamped cosine `max(0, dot(d, direction))` truncated to bands 0..2 and rescaled to that peak.
    /// So it is a half-sphere falloff rather than a disc: a soft key light, never a sharp sun.
    /// A hard sun needs an `area_light` — no order-3 probe can carry one.
    ///
    /// Truncation leaves a floor the clamp would have removed: 3/34 of the peak across the lobe, 1/17 behind it.
    /// So a `sun` also lifts the whole environment a little, and it dips a few percent below zero in the ring between — which the miss's clamp hides rather than fixes.
    [[nodiscard]] static background sun(tg::vec3f direction, tg::vec3f radiance);

    /// A cool blue sky over a dim warm ground bounce, with a soft sun high in the +x/+y quadrant.
    [[nodiscard]] static background daylight();

    /// Neutral gray, brighter overhead: reads shape and material without tinting either.
    [[nodiscard]] static background studio();

    /// This environment layered with `other` — the radiance the two reconstruct, added.
    /// Environments superpose because SH is linear, which is what lets a sky and a sun be authored separately.
    [[nodiscard]] background combined_with(background const& other) const;

    /// This environment at `factor` times the radiance — an exposure knob on a preset.
    [[nodiscard]] background scaled(f32 factor) const;
};

/// GPU-side SH probe, mirroring the `Background` cbuffer in shaders/background.hlsli.
/// Each coefficient sits in its own 16-byte lane (`.xyz` = RGB radiance, `.w` unused), because HLSL pads cbuffer array elements to a full float4 lane.
/// Bound at b1, evaluated by the miss shaders (`background_radiance`).
struct sv::background_gpu
{
    tg::vec4f sh[background::sh_coefficient_count] = {};

    /// Packs a scene-side SH background into its GPU lane layout (each coefficient widened to a vec4, `.w` = 0).
    [[nodiscard]] static background_gpu from(background const& bg)
    {
        auto out = background_gpu{};
        for (auto i = 0; i < background::sh_coefficient_count; ++i)
            out.sh[i] = tg::vec4f(bg.sh[i][0], bg.sh[i][1], bg.sh[i][2], 0.0f);
        return out;
    }
};

namespace sv
{
static_assert(sizeof(background_gpu) == sizeof(tg::vec4f) * background::sh_coefficient_count,
              "background_gpu must be a tight array of vec4 lanes to match the HLSL cbuffer");
} // namespace sv
