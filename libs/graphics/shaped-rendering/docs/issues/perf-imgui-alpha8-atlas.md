---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/imgui_context.cc
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/imgui_texture_registry.cc
  - file: libs/graphics/shaped-rendering/shaders/imgui.sgl
---

# imgui: an Alpha8 font atlas path

`imgui.sgl` samples `.rgba` unconditionally, so `io.Fonts->TexDesiredFormat` is pinned to RGBA32, four times the bytes of Alpha8.
An Alpha8 atlas would render red-on-transparent through today's shader; the texture registry asserts on it.
