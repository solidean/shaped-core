---
created: 2026-10-17
refs:
  - symbol: sr::atrous_denoise_routine
  - file: libs/graphics/shaped-rendering/shaders/atrous_denoise.hlsl
---

# à-trous from a measured per-pixel variance

à-trous estimates noise from the sample count alone, assuming one noise width per sample equal to the pixel's luminance.
A tracer that accumulates the second moment would give it a measured per-pixel variance instead, which is what SVGF uses.
