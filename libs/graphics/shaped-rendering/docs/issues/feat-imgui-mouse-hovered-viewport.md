---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/imgui_context.cc
  - symbol: sr::imgui_context
---

# imgui viewports: `ImGuiBackendFlags_HasMouseHoveredViewport` is not set

So imgui infers the hovered viewport from the mouse position rather than asking the platform.
That is wrong when another application's window sits on top of a viewport.
SDL would have to report the window under the cursor.
