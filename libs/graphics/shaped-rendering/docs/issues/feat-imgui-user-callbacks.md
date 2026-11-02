---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/imgui_routine.cc
  - symbol: sr::imgui_routine
---

# imgui: `ImDrawCmd::UserCallback` is skipped

`imgui_routine` skips a draw command carrying a user callback.
Supporting it also means supporting `ImDrawCallback_ResetRenderState`, which needs the bind block factored out of the draw loop.
No imgui core path emits one, so nothing is lost until a caller adds one.
