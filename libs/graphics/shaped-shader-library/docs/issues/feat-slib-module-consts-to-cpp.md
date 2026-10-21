---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/light.hh
  - file: libs/graphics/shaped-viewer/shaders/sgl/scene_items.sgl
---

# A module's consts reach no C++

slib generates a module's structs and not its consts.
So shaped-viewer restates `scene.light_flag_*` as `sv::light_flag_*` (scene/light.hh) by hand.
