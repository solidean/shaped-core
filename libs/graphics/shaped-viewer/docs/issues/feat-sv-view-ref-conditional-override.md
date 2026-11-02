---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/refs.hh
  - symbol: sv::view_ref
---

# A conditional-override vocabulary on `view_ref`

An `ImGuiCond`-style `when { always, first_use }`, so `camera` / `resolution` / placement stop needing a separate `initial_*` setter each.
Only the first-use half is built today (`initial_camera`, `initial_orbit`, `initial_fps`).
