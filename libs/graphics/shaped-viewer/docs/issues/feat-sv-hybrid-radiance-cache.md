---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/rendering-roadmap.md
---

# A radiance cache, for the interactive hybrid mode

The hybrid rasterizes primary visibility and lights it by tracing; a cache of incoming radiance is what makes that lighting cheap enough per frame.
It is for the interactive mode alone: a final render converges with the wavefront tracer and no cache, so nothing it caches biases a finished image.
Its design comes out of the rendering-modes review in [the roadmap](../plans/rendering-roadmap.md), together with the hybrid's visibility buffer.
