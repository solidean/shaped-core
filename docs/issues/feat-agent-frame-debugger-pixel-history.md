---
created: 2026-11-05
parent: epic-agent-frame-debugger
---

# Pixel history: every draw that touched a pixel, the entry point it ran, its inputs and what it wrote

Per draw: the SGL entry point and its package, the stage inputs the rasterizer interpolated, the depth and stencil tests, and the value written or why none was.
SGL's footprint (`describe`) already says what each entry point reads and writes, which narrows a history to the draws that could have touched the pixel at all.
