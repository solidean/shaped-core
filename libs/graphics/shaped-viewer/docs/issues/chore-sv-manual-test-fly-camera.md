---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/tests/pathtraced-window-manual-test.cc
---

# `pathtraced-window-manual-test` still hand-rolls its own fly camera

A view picks its controller now — `view_ref::camera_style`, with `sv::viewer` routing to it and driving the fly one's `update(dt)`.
The manual test predates that and can drop its own `fly_camera`.
