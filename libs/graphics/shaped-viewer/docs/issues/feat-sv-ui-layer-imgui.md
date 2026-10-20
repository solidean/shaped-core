---
created: 2026-10-17
parent: epic-sv-layout-interaction
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/render_plan.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/layer.hh
---

# The UI layer, through `sr::imgui_context` / `sr::imgui_routine`

`layer_kind::ui` exists and is not drawn yet: `render_plan` skips it.
It should draw through `sr::imgui_context` / `sr::imgui_routine`, into the view's own target.
