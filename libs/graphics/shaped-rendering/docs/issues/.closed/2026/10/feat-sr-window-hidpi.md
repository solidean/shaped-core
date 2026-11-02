---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/window_sdl.cc
  - symbol: sr::window
closed: 2026-10-20
resolution: fixed
---

# HiDPI windows

Sizes are queried in pixels, but `SDL_WINDOW_HIGH_PIXEL_DENSITY` is not requested, so logical and pixel sizes are identical.
Revisit with a per-monitor-DPI UI.
