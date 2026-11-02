---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.hh
  - symbol: sv::view_renderer::execute
---

# `view_renderer::execute`, the single-view entry point, lags the plan path

`view_renderer::execute` is the single-view convenience the GPU tests drive, and it does less than the plan path does.

- **It does not denoise**; only the plan path does.
- **One traced layer per view is still assumed** there: it traces the first scene_3d layer alone.
  The plan and `trace` are already per `(view, layer)`.
