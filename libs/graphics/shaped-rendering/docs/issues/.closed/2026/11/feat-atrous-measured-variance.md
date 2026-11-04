---
created: 2026-10-17
refs:
  - symbol: sr::atrous_denoise_routine
  - file: libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl
  - file: libs/graphics/shaped-rendering/shaders/svgf.sgl
closed: 2026-11-04
resolution: fixed
---

# à-trous from a measured per-pixel variance

à-trous estimates noise from the sample count alone, assuming one noise width per sample equal to the pixel's luminance.
That model, sized by the center pixel, is what darkens a young image and flattens its contrast, `bug-sr-atrous-flattens-young-contrast`.
A measured per-pixel variance is what SVGF steers by, and what this member should steer by too.

The plan, following SVGF (Schied et al. 2017) without its temporal pass:
- sv accumulates the luminance second moment beside the mean, in the accumulated image's alpha, which nothing reads today.
  The megakernel, the wavefront and the hybrid each blend it as they blend the color.
  The variance of the mean is then `(E[l²] - E[l]²) / n` per pixel.
- à-trous steers its luminance stop by that variance, 3x3-blurred, and carries the variance of each pass's average into the next, as `svgf.sgl` does.
  While the mean is young, under four samples, it estimates the variance spatially over 7x7, as SVGF's variance pass does for a short history.
- Before any of it, an anti-firefly clamp as NVIDIA's ReLAX applies: the center clamped to its neighbors' mean plus a few deviations, the center left out of both.
  Without it one extreme sample widens its own window and spreads; `sr - atrous keeps a single extreme sample where it fell` holds that.

Rousselle et al. 2012 is the alternative: per-pixel variance from two half buffers of the mean, which needs no moment but a second accumulation.

Done when the denoiser tests hold à-trous to the other members' 5% brightness and 15% tile limits, which they relax for it today.
