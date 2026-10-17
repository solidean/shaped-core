---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
---

# Wavefront path tracing: one dispatch per bounce?

**A path runs to completion inside one dispatch**, which is what makes a frame cost the DEEPEST path in it rather than the average one.
`path_trace` loops `samples_per_pixel` times over `max_bounces` and a pixel is not done until its whole path is.
So a scene with one deep corner pays for that corner everywhere.
It is the same property that makes a heavy dispatch trip a driver watchdog rather than merely run slowly.

The wavefront answer is one dispatch per bounce, with the path's live state persisted between them rather than held in registers.
That also lets a frame carry a path forward instead of finishing it.

Two things have to be decided before that is worth starting.

- **The state is a G-buffer by another name**: origin, direction, throughput, rng, the medium's three fields, the collapsed wavelength and `prev_pdf`, all per pixel per sample.
  This library has just finished deleting one for the accumulation rework, so the trade wants making deliberately rather than by default.
- **It changes what a frame means to the accumulator.**
  `accum_frame` counts completed estimates, and the mean is exact because every sample folded in is a finished path.
  Either only terminated paths fold in, which makes the effective sample count vary per pixel and gives back the "one eye, one mean" property.
  Or partial radiance folds in, and the target is no longer a mean of anything.
