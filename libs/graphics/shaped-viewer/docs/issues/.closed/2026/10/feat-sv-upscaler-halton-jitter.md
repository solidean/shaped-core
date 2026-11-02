---
created: 2026-10-17
parent: epic-sv-denoising-integration
blocked-by: [feat-sv-render-scale]
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - symbol: sv::view_renderer::_denoise
closed: 2026-10-17
resolution: fixed
---

# A per-frame Halton jitter while an upscaler runs

SVGF does not need one; FSR and DLSS Ray Reconstruction do.
`sr::reconstruct_jitter` hands it out, one offset per frame.

It is distinct from the per-pixel random offset the raygen already applies, whose mean is the pixel centre.
That is why `reconstruct_guides::jitter` is correctly zero today.
What is missing is one offset shared by every pixel of a frame, which is what a temporal upscaler reconstructs sub-pixel detail from.
So it lands with `render_settings::render_scale` rather than before it.
