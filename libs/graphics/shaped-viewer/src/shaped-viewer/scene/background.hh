#pragma once

#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/light.hh>
#include <typed-geometry/linalg/vec.hh>

/// One soft lobe of a dome: `radiance * exp(concentration * (dot(d, direction) - 1))`, peaking at `radiance` along `direction`.
/// That is a von Mises–Fisher lobe, so it has a closed-form integral and closed-form sampling, which is what lets the tracer draw from it exactly.
/// The studio spec's softness of 0.12 is a concentration of 1 / 0.12.
struct sv::dome_lobe
{
    /// From the scene toward the lobe; it must not be zero, and is normalized where it is read.
    tg::vec3f direction = tg::vec3f(0, 1, 0);
    tg::vec3f radiance = tg::vec3f(0, 0, 0);
    f32 concentration = 1.0f / 0.12f;
};

/// An analytic environment: a vertical gradient with up to four soft lobes on it, evaluated and sampled exactly.
///
/// The gradient is `horizon` at and below the elevation whose sine is `rise_start`, and rises by a smoothstep to `zenith` straight up.
/// A camera ray that misses sees the gradient alone, times `backdrop_scale`, so no lobe ever shows in the backdrop; every other ray sees the whole dome.
struct sv::dome
{
    static constexpr int max_lobes = 4;

    tg::vec3f horizon = tg::vec3f(0, 0, 0);
    tg::vec3f zenith = tg::vec3f(0, 0, 0);
    f32 rise_start = 0.0f;
    f32 backdrop_scale = 1.0f;

    dome_lobe lobes[max_lobes] = {};
    int lobe_count = 0;

    /// The gradient alone along unit direction `d`.
    [[nodiscard]] tg::vec3f gradient(tg::vec3f d) const;

    /// The whole dome along unit direction `d`: the gradient plus every lobe.
    [[nodiscard]] tg::vec3f radiance(tg::vec3f d) const;
};

/// The view's background / environment: the radiance a primary ray sees when it misses all geometry.
///
/// It is an order-3 RGB spherical-harmonics probe — 16 coefficients, each an RGB radiance, in the standard real-SH basis with index 0 the constant (DC) term.
/// Or it is an analytic `dome`, which every renderer evaluates and samples exactly; `sh` then holds the dome's projection, for what reads SH alone.
/// Order 3 captures a sky gradient plus some soft directional structure, which is all a background needs.
/// A sun is not one of those: it is a light (`sv::light::sun`), and `sv::daylight()` hands the two back as a pair.
/// A miss shader reconstructs the radiance along a ray direction from these coefficients.
///
/// All-zero is a black background.
/// The coefficients are raw SH, so writing one by hand means knowing the basis — the factories below are the way in.
/// They compose, because SH is linear: `gradient(...).combined_with(lobe(...))` reconstructs the sky plus the lobe.
struct sv::background
{
    static constexpr int sh_coefficient_count = 16; // order 3: (3 + 1)^2 real-SH coefficients

    tg::vec3f sh[sh_coefficient_count] = {};

    /// Set for an analytic dome, which then defines the environment; `sh` is its projection.
    bool is_dome = false;
    sv::dome analytic = {};

    /// The analytic `d`, with `sh` projected from it.
    [[nodiscard]] static background of_dome(sv::dome const& d);

    /// The same `radiance` in every direction — a flat ambient environment, band 0 alone.
    [[nodiscard]] static background uniform(tg::vec3f radiance);

    /// A vertical gradient: `zenith` looking straight up (+y), `nadir` straight down, and their average on the horizon.
    /// Exact rather than fitted — a radiance linear in the up component lives entirely in bands 0 and 1.
    [[nodiscard]] static background gradient(tg::vec3f zenith, tg::vec3f nadir);

    /// A soft directional lobe peaking at exactly `radiance` along `direction`, which must not be zero (it is normalized here) and points from the scene toward the lobe.
    ///
    /// The shape is the clamped cosine `max(0, dot(d, direction))` truncated to bands 0..2 and rescaled to that peak.
    /// So it is a half-sphere falloff rather than a disc: a soft key or fill, and **never a sun** — it casts no shadow and
    /// reflects as a smear.
    /// A sun is a light, `sv::light::sun`, since no order-3 probe can carry one.
    ///
    /// Truncation leaves a floor the clamp would have removed: 3/34 of the peak across the lobe, 1/17 behind it.
    /// So a `lobe` also lifts the whole environment a little, and it dips a few percent below zero in the ring between — which the miss's clamp hides rather than fixes.
    [[nodiscard]] static background lobe(tg::vec3f direction, tg::vec3f radiance);

    /// Neutral gray, brighter overhead: reads shape and material without tinting either.
    [[nodiscard]] static background studio();

    /// This environment layered with `other` — the radiance the two reconstruct, added.
    /// Environments superpose because SH is linear, which is what lets a sky and a sun be authored separately.
    /// Neither may be a dome.
    [[nodiscard]] background combined_with(background const& other) const;

    /// This environment at `factor` times the radiance — an exposure knob on a preset.
    [[nodiscard]] background scaled(f32 factor) const;
};

/// A sky and the sun that lights it, authored as a pair — what `sv::daylight()` hands back.
///
/// **The sky holds no sun**, so the two are never counted twice: the sun's light comes from `sun` alone, sharp and sampled,
/// and the sky is only what surrounds it.
/// Set both — `scene.background(d.sky)` and `scene.add_light("sun", d.sun)` — since half of the pair is not daylight.
struct sv::sky_and_sun
{
    background sky;
    light sun;
};

namespace sv
{
/// A cool blue sky over a dim warm ground bounce, with a warm sun high in the +x/+y quadrant.
[[nodiscard]] sky_and_sun daylight();
} // namespace sv

/// The environment as the GPU reads it, `traced.background` and `drawn.background`: 28 `float4` lanes, which module `scene` takes as `float4[28]`.
///
/// The 16 SH lanes come first (`.xyz` = RGB radiance), so a reader of the probe alone reads them as it always has.
/// The dome follows: a header (kind, lobe count, the rise's start, the backdrop scale), the horizon and the zenith, then per lobe its
/// direction and concentration, and its peak radiance and the probability of sampling it; the last lane holds the gradient's probability.
/// `scene.environment_*` (shaders/sgl/scene_items.sgl) evaluates and samples it.
struct sv::background_gpu
{
    static constexpr int lane_count = 28;

    tg::vec4f sh[background::sh_coefficient_count] = {};
    tg::vec4f header = {}; ///< x: 1 for a dome, 0 for a probe; y: lobe count; z: the rise's start; w: the backdrop scale
    tg::vec4f horizon = {};
    tg::vec4f zenith = {};
    tg::vec4f lobe_direction[dome::max_lobes] = {}; ///< xyz: unit direction; w: concentration
    tg::vec4f lobe_radiance[dome::max_lobes] = {};  ///< xyz: peak radiance; w: probability a sample picks this lobe
    tg::vec4f gradient = {}; ///< x: probability a sample picks the gradient, drawn uniformly over the sphere

    /// Packs `bg` into its lanes, with each part of a dome given its share of the samples by its power.
    [[nodiscard]] static background_gpu from(background const& bg);
};

namespace sv
{
static_assert(sizeof(background_gpu) == sizeof(tg::vec4f) * background_gpu::lane_count,
              "background_gpu must be a tight array of vec4 lanes, as traced.background reads it");
} // namespace sv
