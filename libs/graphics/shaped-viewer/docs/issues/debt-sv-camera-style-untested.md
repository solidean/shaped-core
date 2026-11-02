---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/tests/camera-controller-test.cc
  - symbol: sv::view_ref::camera_style
---

# Nothing tests `camera_style`

The controllers have their own CPU tests.
The switch, the re-seeding across it and the unfocused release all live in `sv::viewer`, which needs a window.
So they are only exercised by running an example.
