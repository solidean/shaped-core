---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/light.hh
  - symbol: link_mask
---

# Light linking is reserved, not built

Both GPU records carry a mask, all ones, and nothing reads it yet.
Building it is testing the light's mask against the hit instance's in both estimators, plus an authoring call that sets them.
