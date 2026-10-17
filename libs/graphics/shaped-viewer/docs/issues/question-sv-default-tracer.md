---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/docs/wavefront.md
  - symbol: sv::render_mode
---

# Which tracer is the default

A megakernel frame costs its deepest path, since a path runs to completion inside one dispatch, which is also what lets a heavy dispatch trip a driver watchdog.
The wavefront tracer answers it and is the default today; the megakernel stays as `render_mode::megakernel`, because on NVIDIA it is faster on shallow paths.
What is open is the default itself: very likely the hybrid, lit by the megakernel, with the wavefront as the best-quality mode — [wavefront.md](../wavefront.md#which-tracer-is-the-default).
