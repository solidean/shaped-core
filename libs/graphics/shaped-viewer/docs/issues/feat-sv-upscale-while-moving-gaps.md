---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - symbol: sv::view_renderer::trace
  - test: sv - a moving camera traces at the upscaler's extent and a still one at full size
---

# What upscaling while the image changes still leaves out

Upscaling runs only while the image changes, which is `reconstruct.scale` below native: a changed frame traces at the input extent with one jitter, and a still one restarts at full size.

- **A layer with drawings or annotations never upscales**, because their depth test reads the primary depth at the view's size; a full-size depth from the scaled frame would lift it.
- **The first still frame starts the full-size temporal history over**, since that history last saw the camera before the move, so stopping shows one frame denoised from a single sample.
- **`dlss_rr` is reached, but only a hybrid layer's test runs it**: `sv - a moving hybrid layer draws at the upscaler's extent and a still one at full size` does, since 2026-10-28.
  A traced layer takes the same path through `trace`, untested against it.
- **`frame_time_ms` is the default 60 Hz**, not the viewer's own frame time.
