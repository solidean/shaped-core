---
created: 2026-11-03
refs:
  - file: libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl
  - test: sv agreement - every denoiser keeps the image's brightness and brings it closer to the converged one
---

# À-trous flattens a young image's contrast at the scale of its widest pass

On the Cornell box at four samples a pixel, behind the megakernel and behind the hybrid, à-trous moves light from the lit back wall into the ceiling's corners.
A 16x16 tile at the top corner comes out at 0.256 where it converges to 0.209, and one on the wall under the lamp at 0.523 where it converges to 0.600.
That is a fifth of the frame's mean brightness moved between tiles; SVGF moves at most 14% and OIDN and DLSS-RR under 9%.
The image's overall brightness is right, within 4%, since its luminance window became symmetric.

Measured, not explained yet.
The normal edge-stop (power 64) keeps wall and ceiling apart, so the light moves along one surface.
A likely cause is the window's noise model: one noise width per sample, equal to the brighter pixel's luminance.
That lets a stray bright sample near the lamp spread along the ceiling as far as the last pass reaches.
`feat-atrous-measured-variance` is the usual answer to a crude noise model, and would be the first thing to try.
