---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/layer.hh
  - symbol: sv::view_ref::add_scene
---

# A traced layer has no alpha, so two scene layers cannot composite

The raygen in `tracer_pipeline.sgl` writes none, so a `scene_3d` layer is forced to `layer_blend::replace`.
Writing coverage into `.a` is what would let a traced layer composite `over` another.
Until then `view_ref::add_scene` can express two scene layers on one view but only the last is visible.
