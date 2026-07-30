#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// The view's background / environment: the radiance a primary ray sees when it misses all geometry.
///
/// For now it is an order-3 RGB spherical-harmonics probe — 16 coefficients, each an RGB radiance, in the standard real-SH basis with index 0 the constant (DC) term.
/// Order 3 captures a sky gradient plus some directional structure (a soft sun disc, horizon banding), which is all a background needs.
/// A miss shader reconstructs the radiance along a ray direction from these coefficients.
///
/// All-zero is a black background.
/// This is the scene-side description; the path tracer's miss does not read it yet — it lands with the pt environment.
struct background
{
    static constexpr int sh_coefficient_count = 16; // order 3: (3 + 1)^2 real-SH coefficients

    tg::vec3f sh[sh_coefficient_count] = {};
};

/// GPU-side SH probe, mirroring the `Background` cbuffer in shaders/background.hlsli.
/// Each coefficient sits in its own 16-byte lane (`.xyz` = RGB radiance, `.w` unused), because HLSL pads cbuffer array elements to a full float4 lane.
/// Bound at b1, evaluated by the miss shaders (`background_radiance`).
struct background_gpu
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

static_assert(sizeof(background_gpu) == sizeof(tg::vec4f) * background::sh_coefficient_count,
              "background_gpu must be a tight array of vec4 lanes to match the HLSL cbuffer");
} // namespace sv
