---
created: 2026-10-17
refs:
  - symbol: sr::svgf_denoise_routine
  - file: libs/graphics/shaped-rendering/shaders/svgf.sgl
  - test: sr - svgf*
---

# SVGF reprojects from the nearest texel

SVGF's reprojection takes the nearest texel rather than a bilinear footprint, which smears slightly under subpixel motion.

A second departure from the paper, chosen rather than pending: SVGF feeds back its integrated, unfiltered colour, where the paper feeds back the first à-trous pass's output.
That is simpler and never compounds the filter across frames, at the price of a noisier history.
`svgf_denoise_routine.hh` documents it; revisit it together with the footprint if the history's noise shows.
