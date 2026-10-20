---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/annotation.hh
  - file: libs/graphics/shaped-viewer/docs/canvas.md
---

# Annotations do not avoid each other

Each is placed on its own, so labels whose anchors are close overlap.
Decluttering is greedy in priority order against the previous frame's boxes.
That wants the boxes to persist across frames and an identity per annotation.
