---
created: 2026-11-01
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_wavefront.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
closed: 2026-11-01
resolution: fixed
---

# Denoiser guides follow the stochastic cutout, so a temporal denoiser keeps partial-coverage noise as detail

A material with an opacity below 1 is a stochastic cutout (`tracer.cutout`): each ray takes or skips the surface, which converges to a partly covered one.
The guides are summed from each path's primary hit (`tracer_wavefront.sgl`, the `path_flag_primary` branch of the shade), so at a partial coverage they are of whichever surface that sample took.

## Measured

On an RTX 5070 Ti at 1080p, a 0.3-opacity thin-walled openpbr shell over opaque geometry, on `denoise_method::automatic`:

- The temporal member that runs is `dlss_rr`, status ok, every frame; no refusal.
- The albedo guide behind the shell is salt and pepper, shell white against the dark surface behind it, pixel by pixel.
  The raw frame carries the same pattern, so the colour's noise is fully explained by the albedo.
- The presented image is clean on opaque surfaces and raw noise wherever a shell covers something, unchanged from 1 to 32 samples a frame.
- The same scene converges without a denoiser (clean at 2048 samples a pixel), and OIDN on the mean of 32 is close to that: the noise is sampling noise, and only the temporal path keeps it.

## Why

A temporal reconstruction keeps what its guides explain and removes what they do not.
A guide that flips with the cutout explains the flip, so the member keeps it, and depth and motion that jump between two surfaces reject the history as well.

## Fix directions, not chosen

- Guides from the first fully covered surface, by a guide ray that skips every partial coverage: deterministic guides, and the shell becomes part of the signal the member averages.
  Costs one primary ray a pixel where a scene holds a partial coverage; motion is the surface behind, so a shell moving over a still background lags.
- Guides as the coverage-weighted blend of the shell and what is behind it: closer to what the image shows, and needs the shell's coverage along the guide ray, which an any-hit visits unordered.
- Partial coverage composited after the denoise, as raster engines do with transparents: the cleanest image, the largest change.
