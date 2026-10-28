---
created: 2026-10-28
---

# Studio mode: view-following softbox dome, diffuse floor, fixed exposure and a neutral tone curve

**Wanted: single-asset renders lit as Philip's studio spec describes**, https://gist.github.com/Philip-Trettner/9001008f98e280e45c0dd1d97a3813e0.
Asked for on 2026-10-28, after the hybrid renderer's current work.

What the spec asks for, and what sv lacks for each:

- **A dome with three soft lobes**, key, fill and rim, each `strength * H * exp((dot(d, L) - 1) / 0.12)` over a horizon-to-zenith gradient.
  sv's environment is 16 SH coefficients, which blur a ~23° lobe away.
  **This wants an image-based environment, sampled by its radiance, in the tracer and the hybrid alike**: the real library extension, and worth having for any HDRI.
- **The lobes follow the camera**, so every view of a sheet is lit alike: the dome is rebuilt when the view moves.
- **The camera sees the dome at 2.4 H, and lighting at H**, so the horizon blends into the floor without a line.
  sv has one radiance for both.
- **An infinite floor at y = 0**, fully diffuse, albedo 0.35.
- **Fixed exposure, EV -8.3**, and the "neutral" display transform: linear to 0.6, a hue-preserving shoulder on the peak channel above it, then the exact sRGB encode.
  Whether `sr`'s tonemapping has these is not checked yet.
- **The camera**: 50 mm on a 36 mm sensor, framed at true scale from the asset's bounds, and the six standard sheet views.
- **A ceiling variant** for hanging assets: no floor, a dark zenith, the rise from the horizon, the key below it, EV -8.48.

Calibration: a 0.19-albedo grey card square to the key renders at sRGB 122.
