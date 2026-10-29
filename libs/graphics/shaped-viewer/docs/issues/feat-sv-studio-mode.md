---
created: 2026-10-28
---

# Studio mode: view-following softbox dome, diffuse floor, fixed exposure and a neutral tone curve

**Wanted: single-asset renders lit as Philip's studio spec describes**, https://gist.github.com/Philip-Trettner/9001008f98e280e45c0dd1d97a3813e0.
Asked for on 2026-10-28, after the hybrid renderer's current work.

What the spec asks for, and what sv lacks for each:

- **A dome with three soft lobes**, key, fill and rim, each `strength * H * exp((dot(d, L) - 1) / 0.12)` over a horizon-to-zenith gradient.
  sv's environment is 16 SH coefficients, which blur a ~23° lobe away.
  Each lobe is a von Mises–Fisher lobe of concentration 1 / 0.12, so the dome has a closed form and closed-form sampling.
- **The lobes follow the camera**, so every view of a sheet is lit alike: the dome is rebuilt when the view moves.
- **The camera sees the dome at 2.4 H, and lighting at H**, so the horizon blends into the floor without a line.
  sv has one radiance for both.
- **An infinite floor at y = 0**, fully diffuse, albedo 0.35.
- **Fixed exposure, EV -8.3**, and the "neutral" display transform: linear to 0.6, a hue-preserving shoulder on the peak channel above it, then the exact sRGB encode.
  Whether `sr`'s tonemapping has these is not checked yet.
- **The camera**: 50 mm on a 36 mm sensor, framed at true scale from the asset's bounds, and the six standard sheet views.
- **A ceiling variant** for hanging assets: no floor, a dark zenith, the rise from the horizon, the key below it, EV -8.48.

Calibration: a 0.19-albedo grey card square to the key renders at sRGB 122.

## The design

Settled in a design review on 2026-10-29.

**The shaders reach the environment through one interface**: its radiance along a direction, a sampled direction with its pdf, and the pdf of a given direction.
Every miss, every environment sample and every MIS weight against the environment goes through it, in the tracer, the wavefront, the hybrid and the raster alike.
**The studio dome is an analytic kind behind it**: a gradient (horizon, zenith, the sine of the elevation the rise starts at).
On it sit up to four vMF lobes, each a direction, a radiance and a concentration.
Today's SH probe is the other kind; an image environment for HDRIs is a third, filed as [feat-sv-image-environment](feat-sv-image-environment.md).

**One sample per point, from a power-weighted mixture**: a lobe with probability by its power, `strength · 2π (1 − e^(−2κ)) / κ`, the gradient by its integral and drawn cosine-weighted.
The pdf is the mixture's, which is also what a bounce's MIS weight reads.
A test integrates the dome by sampling and checks it against its closed form.

**A camera ray that misses sees the dome's gradient times 2.4, without its lobes**: the spec's "2.4 × H" at the horizon, the dome's rise kept, and no lobe ever in the backdrop.
Only a ray that left the camera: the tracer's first segment, and the raster's environment pass; reflections and bounces see the dome as it is.

**The floor is an ordinary scene item**: a two-triangle quad at y = 0 under the asset, a few hundred times its extent, openpbr with `base_color` 0.35 and `specular_weight` 0.

**Exposure and the curve are applied per view in sv's layout pass**: a view carries an exposure in stops and a curve, none or neutral, and `view_ps` applies them and encodes unless the target does.
The neutral curve and the sRGB encode are an SGL module shaped-rendering exports.
Tests pin its reference points: 18% grey reads sRGB 118, linear 0.6 reads 203, and the knee has slope 1.

**The lobes follow the camera every frame**, and a setting freezes them where they are.

**Public pieces, and a `studio` preset composed from them**: the dome environment, the floor helper, the view's exposure and curve, and `scene.studio({...})` setting all of them to the spec's values.

**The first version also carries**:
- the exposure derived from the dome for H = 1, pinned by a test rendering a 0.19 grey card square to the key at sRGB 122;
- an `sv::camera` helper framing an asset from its bounds at 50 mm on a 36 mm sensor, for the six sheet views;
- the ceiling variant, its own calibration included.
