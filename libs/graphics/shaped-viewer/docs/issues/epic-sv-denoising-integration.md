---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/render_settings.hh
---

# Denoising in sv: both halves run, what is left

Both halves of denoising run in sv; what follows is what is left, in order.
shaped-rendering's [reconstruction.md](../../../shaped-rendering/docs/reconstruction.md) is the design.

A camera move throws the whole accumulated estimate away, which is the deliberate trade behind an uncapped exact mean, and it is what makes flying noisy.
Reprojecting the history through the previous camera and rejecting per pixel is the classical answer and was tried.
It cost a G-buffer, a ping-pong pair, a disocclusion heuristic and a per-pixel sample count, and it capped the mean.
A denoiser buys the same smoothness without touching the estimator, and that is the direction taken: this epic is it.
