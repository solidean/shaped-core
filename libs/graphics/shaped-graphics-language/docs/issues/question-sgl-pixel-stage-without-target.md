---
created: 2026-10-30
corpus:
  - bgfx/shaders/21-deferred/fs_deferred_clear_uav
  - bgfx/shaders/21-deferred/fs_deferred_light_uav
---

# A pixel stage that writes only images and no render target

CHK-91 has every `@pixel fun` return a `@pixel struct`, so a pixel stage whose whole effect is an image store has no spelling.
bgfx's deferred example clears and lights its g-buffer that way, through UAVs, with no color target bound.
Every target can express it: HLSL and MSL as a void fragment function, WGSL as a fragment with no output.

- **Shapes to weigh**: a `@pixel fun` with no result, or an empty `@pixel struct`; the first reads as what it is, the second keeps CHK-91.
- What the pipeline description makes of it: a raster pipeline with no color target, which sg may need to allow as well.
- The two translations write the first shape, marked `// gap:`, and compile once a dummy target stands in, so nothing else blocks them.
