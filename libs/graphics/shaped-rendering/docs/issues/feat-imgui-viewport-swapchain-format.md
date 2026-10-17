---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/imgui_routine.cc
  - symbol: sr::imgui_routine
---

# imgui viewports: a viewport swapchain is pinned to `bgra8_unorm`

A viewport swapchain does not match the caller's main swapchain, whose format the routine never learns.
Not an sRGB format on purpose: imgui's colors are already sRGB-encoded, so an sRGB target would encode them twice.
Worth threading the format through once a caller wants a different one.
