---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/camera_controller.hh
---

# A view whose cursor leaves the window mid-look keeps looking

The fly controller releases input while the window is unfocused.
Losing the CURSOR is still unhandled — only focus is.
