---
created: 2026-11-03
refs:
  - file: libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl
  - test: sv agreement - every denoiser keeps the image's brightness and brings it closer to the converged one
---

# À-trous darkens a young image and flattens its contrast

On the Cornell box at four samples a pixel, à-trous comes out 8% darker than the raw mean behind the megakernel and 15% behind the hybrid.
It also moves light from the lit back wall into the ceiling's corners, a fifth of the frame's mean brightness between 16x16 tiles.
SVGF, OIDN and DLSS-RR keep the brightness within 2% and move at most 14% between tiles.

The darkening is the luminance window, sized by the center pixel alone: `scale * (|center| + 0.01)`.
A dark pixel's window is narrow and refuses the bright samples beside it, while a bright pixel's is wide and takes the dark ones in.
So energy only ever moves downward.

Sizing the window by the brighter pixel of each pair removed the darkening, and was reverted.
Such a window can never refuse a tap brighter than the center, so a one-sample frame's brightest sample spread over half the image, a block that jumped from frame to frame.
`sr - atrous keeps a single extreme sample where it fell` holds that now.

A fix has to keep refusing outliers while letting a dark pixel take in its share of bright noise.
That wants a noise width measured rather than modeled from the center, `feat-atrous-measured-variance`, as SVGF's is.
The flattening is measured and not explained yet; the same noise model, letting a stray bright sample near the lamp spread along the ceiling, is the likely cause.
